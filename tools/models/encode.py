"""Build an Enshrouded RenderModel render-data blob and the record fields that describe it (inverse of rendermodel.py).

Input: vertex groups. A group is one material's vertex set; its LODs are triangle lists over the same vertices
(the game shares one vertex set across LODs). Positions, normals and tangents are in GAME space (left-handed).

  group = {"material": int, "pos": [(x,y,z)], "nrm": [(x,y,z)], "tan": [(x,y,z,w)], "col": [(r,g,b,a)] or None,
           "uv": [(u,v)], "skin": [([game joint, ...], [weight, ...])] or None, "lods": [[(a,b,c), ...] per LOD]}

build(groups, palette=None, box=None) -> (blob bytes, fields dict) where fields holds everything of the RenderModel
record that depends on the geometry (meshes, lods, ranges, bounds, quantisation box, skinningMap, jointCount).
"""
import math
import struct

M21 = (1 << 21) - 1
MAX_VERTS = 65535  # u16 indices per mesh


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def pack_position(p, offset, scale):
    q = [_clamp(int(round((p[k] - offset[k]) / scale[k] * M21)) if scale[k] else 0, 0, M21) for k in range(3)]
    qx, qy, qz = q
    w0 = (qx << 11) | (qy >> 10)
    w1 = qz | ((qy & 0x3FF) << 21)
    return struct.pack("<II", w0, w1)


def _snorm10(v):
    return int(round(_clamp(v, -1.0, 1.0) * 511.0)) & 0x3FF


def pack_normal(n):
    return struct.pack("<I", _snorm10(n[0]) | (_snorm10(n[1]) << 10) | (_snorm10(n[2]) << 20) | (1 << 30))


def pack_tangent(t):
    w = 3 if t[3] < 0 else 1  # 2-bit snorm: 01 = +1, 11 = -1
    return struct.pack("<I", _snorm10(t[0]) | (_snorm10(t[1]) << 10) | (_snorm10(t[2]) << 20) | (w << 30))


def pack_weights(weights):
    """Up to 4 weights -> 4 bytes summing to exactly 255 (largest remainder)."""
    w = list(weights[:4]) + [0.0] * (4 - len(weights[:4]))
    total = sum(w) or 1.0
    scaled = [x / total * 255.0 for x in w]
    q = [int(math.floor(x)) for x in scaled]
    rest = 255 - sum(q)
    order = sorted(range(4), key=lambda i: scaled[i] - q[i], reverse=True)
    for i in order[:rest]:
        q[i] += 1
    return q


def top4(joints, weights):
    pairs = sorted(zip(weights, joints), key=lambda p: -p[0])
    pairs = [p for p in pairs if p[0] > 0][:4] or [(1.0, joints[0] if joints else 0)]
    return [j for _, j in pairs], [w for w, _ in pairs]


def bounds(points):
    lo = [min(p[k] for p in points) for k in range(3)]
    hi = [max(p[k] for p in points) for k in range(3)]
    return lo, hi


def sphere(points):
    lo, hi = bounds(points)
    c = [(lo[k] + hi[k]) / 2 for k in range(3)]
    r = max(math.sqrt(sum((p[k] - c[k]) ** 2 for k in range(3))) for p in points)
    return c, r


def build(groups, palette=None, box=None):
    for g in groups:
        if len(g["pos"]) > MAX_VERTS:
            raise ValueError("a material group has %d vertices; at most %d (split the mesh)" % (len(g["pos"]), MAX_VERTS))
    allp = [p for g in groups for p in g["pos"]]
    lo, hi = bounds(allp)
    if box:
        offset, scale = box
    else:
        offset = lo
        scale = [max(hi[k] - lo[k], 1e-6) for k in range(3)]
    skinned = any(g.get("skin") for g in groups)
    if skinned and palette is None:
        used = sorted({j for g in groups for joints, w in g["skin"] for j, x in zip(*top4(joints, w))})
        palette = used
    pal_index = {j: i for i, j in enumerate(palette or [])}
    if len(pal_index) > 256:
        raise ValueError("more than 256 joints used")

    vertex = bytearray()
    group_vbyte, group_skin = [], []
    skin = bytearray()
    skin_vertex = 0
    for g in groups:
        n = len(g["pos"])
        group_vbyte.append(len(vertex))
        vertex += b"".join(pack_position(p, offset, scale) for p in g["pos"])
        vertex += b"".join(pack_normal(v) for v in g["nrm"])
        vertex += b"".join(pack_tangent(t) for t in g["tan"])
        cols = g.get("col") or [(255, 255, 255, 255)] * n
        vertex += b"".join(bytes(c) for c in cols)
        vertex += b"".join(struct.pack("<2e", *t) for t in g["uv"])
        group_skin.append(skin_vertex)
        if skinned:
            for joints, weights in g["skin"]:
                js, ws = top4(joints, weights)
                q = pack_weights(ws)
                ids = [pal_index[j] for j in js] + [0] * (4 - len(js))
                # keep the game's habit: unused slots repeat index 0 with weight 0
                skin += bytes(ids) + bytes(q)
            skin_vertex += n

    nlods = max(len(g["lods"]) for g in groups)
    meshes, lods, index = [], [], []
    for lod in range(nlods):
        first = len(meshes)
        for gi, g in enumerate(groups):
            if lod >= len(g["lods"]) or not g["lods"][lod]:
                continue
            tris = g["lods"][lod]
            pts = [g["pos"][i] for t in tris for i in t]
            c, r = sphere(pts)
            meshes.append({
                "boundingSphere": {"center": {"x": c[0], "y": c[1], "z": c[2]}, "radius": r},
                "materialIndex": g["material"], "vertexCount": len(g["pos"]), "indexOffset": len(index),
                "indexCount": len(tris) * 3, "meshletOffset": 0, "meshletCount": 0,
                "vertexDataByteOffset": group_vbyte[gi], "blendShapeByteOffset": 0,
                "skinVertexOffset": group_skin[gi] if skinned else 0, "vertexFormat": "PNTCT", "vertexStride": 24,
                "clothMeshIndex": 0, "positionQuantization": "x21y21z21_unorm"})
            index += [i for t in tris for i in t]
        lods.append({"meshOffset": first, "meshCount": len(meshes) - first, "pixelSize": 0.0})

    blob = bytearray(vertex)
    skin_start = len(blob)
    blob += skin
    index_start = len(blob)
    blob += struct.pack("<%dH" % len(index), *index)
    end_of_data = len(blob)
    while len(blob) % 4:
        blob.append(0)
    c, r = sphere(allp)
    fields = {
        "boundingBox": {"min": dict(zip("xyz", lo)), "max": dict(zip("xyz", hi))},
        "boundingSphere": {"center": dict(zip("xyz", c)), "radius": r},
        "positionScale": dict(zip("xyz", scale)), "positionOffset": dict(zip("xyz", offset)),
        "meshes": meshes, "lods": lods,
        "skinningMap": list(palette or []), "jointCount": len(palette or []),
        "vertexData": {"start": 0, "count": len(vertex)},
        "skinningData": {"start": skin_start, "count": len(skin)},
        "indexData": {"start": index_start, "count": end_of_data - index_start},
        "meshletData": {"start": len(blob), "count": 0},
        "blendShapeData": {"start": 0, "count": 0},
    }
    return bytes(blob), fields


def groups_from_model(model, blob):
    """Decode a game model into vertex groups (for round trips and for re-using a game body)."""
    import rendermodel as rm
    groups = {}
    for lod_i, lod in enumerate(model["lods"]):
        for mi in range(lod["meshOffset"], lod["meshOffset"] + lod["meshCount"]):
            mesh = model["meshes"][mi]
            d = rm.decode_mesh(model, blob, mi)
            key = mesh["vertexDataByteOffset"]
            if key not in groups:
                groups[key] = {"material": mesh["materialIndex"], "pos": d["pos"], "nrm": d["nrm"], "tan": d["tan"],
                               "col": d["col"], "uv": d["uv"], "skin": d["skin"] or None, "lods": []}
            g = groups[key]
            while len(g["lods"]) < lod_i:
                g["lods"].append([])
            g["lods"].append(d["tris"])
    return [groups[k] for k in sorted(groups)]
