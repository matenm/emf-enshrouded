"""Decode an Enshrouded keen::RenderModel (record JSON + render-data blob) and export OBJ / a preview PNG.

Inputs come from mods/model_survey (EML export): <name>.json is tostring(RenderModel) and
<name>.render.bin is the content blob named by RenderModel.renderData.hash.

Blob layout (checked on pet_dog_01, build 1273043):
  position:     two u32 w0, w1: x = w0 >> 11, z = w1 & 0x1FFFFF, y = (w0 & 0x7FF) << 10 | (w1 >> 21) & 0x3FF,
                each / (2^21 - 1) * positionScale + positionOffset (reproduces boundingBox exactly); w1 bit 31 unused
  vertexData:   per mesh, at vertexDataByteOffset, separate streams for that mesh's n vertices:
                positions n*8 | normals n*4 | tangents n*4 | colours n*4 | uvs n*4   (= n*24, "PNTCT/24")
  skinningData: 8 bytes per vertex (4 x u8 joint index into skinningMap, 4 x u8 weight summing to 255),
                indexed by mesh.skinVertexOffset + local vertex index
  indexData:    u16 triangle lists, mesh-local, mesh.indexOffset counted in indices
  LODs share the vertex streams and only change the index lists.

Usage: python -I rendermodel.py <dump_dir>/<name> [--lod 0] [--obj out.obj] [--png out.png]
"""
import argparse
import json
import math
import re
import struct
import zlib

M21 = (1 << 21) - 1


def load_record(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"MappedVariantValue: [0-9a-fA-F]+", "null", text)
    text = re.sub(r"(?<=[:\[,])nil(?=[,\]}])", "null", text)
    return json.loads(text)


def snorm10(v):
    return (v - 1024 if v >= 512 else v) / 511.0


def decode_mesh(model, blob, mi):
    mesh = model["meshes"][mi]
    if mesh["vertexFormat"] != "PNTCT" or mesh["positionQuantization"] != "x21y21z21_unorm":
        raise ValueError("unsupported mesh format %s/%s" % (mesh["vertexFormat"], mesh["positionQuantization"]))
    n = mesh["vertexCount"]
    base = model["vertexData"]["start"] + mesh["vertexDataByteOffset"]
    sc, of = model["positionScale"], model["positionOffset"]
    pos, nrm, tan, col, uv = [], [], [], [], []
    for i in range(n):
        w0, w1 = struct.unpack_from("<II", blob, base + i * 8)
        qx = w0 >> 11
        qz = w1 & M21
        qy = ((w0 & 0x7FF) << 10) | ((w1 >> 21) & 0x3FF)  # w1 bit 31 is unused
        pos.append((of["x"] + qx / M21 * sc["x"], of["y"] + qy / M21 * sc["y"], of["z"] + qz / M21 * sc["z"]))
    o = base + n * 8
    for i in range(n):
        w, = struct.unpack_from("<I", blob, o + i * 4)
        nrm.append(tuple(snorm10((w >> (10 * k)) & 1023) for k in range(3)))
    o += n * 4
    for i in range(n):
        w, = struct.unpack_from("<I", blob, o + i * 4)
        tan.append(tuple(snorm10((w >> (10 * k)) & 1023) for k in range(3)) + (-1.0 if w >> 31 else 1.0,))
    o += n * 4
    for i in range(n):
        col.append(tuple(blob[o + i * 4: o + i * 4 + 4]))
    o += n * 4
    for i in range(n):
        uv.append(struct.unpack_from("<2e", blob, o + i * 4))
    skin = []
    if model["jointCount"]:
        so = model["skinningData"]["start"] + mesh["skinVertexOffset"] * 8
        for i in range(n):
            j = blob[so + i * 8: so + i * 8 + 4]
            w = blob[so + i * 8 + 4: so + i * 8 + 8]
            skin.append(([model["skinningMap"][k] for k in j], [x / 255.0 for x in w]))
    idx = struct.unpack_from("<%dH" % mesh["indexCount"], blob, model["indexData"]["start"] + mesh["indexOffset"] * 2)
    tris = [idx[t:t + 3] for t in range(0, len(idx) - 2, 3)]
    return {"material": mesh["materialIndex"], "pos": pos, "nrm": nrm, "tan": tan, "col": col, "uv": uv,
            "skin": skin, "tris": tris}


def lod_meshes(model, lod):
    entry = model["lods"][lod]
    return range(entry["meshOffset"], entry["meshOffset"] + entry["meshCount"])


def write_obj(model, meshes, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# %s (Enshrouded RenderModel, decoded)\n" % model["debugName"])
        vbase = 1
        for k, m in enumerate(meshes):
            f.write("o mesh%d_material%d\n" % (k, m["material"]))
            for p in m["pos"]:
                f.write("v %.6f %.6f %.6f\n" % p)
            for t in m["uv"]:
                f.write("vt %.6f %.6f\n" % (t[0], 1.0 - t[1]))
            for v in m["nrm"]:
                f.write("vn %.5f %.5f %.5f\n" % v)
            for a, b, c in m["tris"]:
                a, b, c = a + vbase, b + vbase, c + vbase
                f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a, a, a, b, b, b, c, c, c))
            vbase += len(m["pos"])


def write_png(meshes, path, width=900, height=640, yaw_deg=60.0):
    """Tiny z-buffered rasteriser (orthographic, Lambert shading) so no extra packages are needed."""
    cy, sy = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    pts = []
    for m in meshes:
        pts.append([(p[0] * cy + p[2] * sy, p[1], -p[0] * sy + p[2] * cy) for p in m["pos"]])
    allp = [p for ps in pts for p in ps]
    minx, maxx = min(p[0] for p in allp), max(p[0] for p in allp)
    miny, maxy = min(p[1] for p in allp), max(p[1] for p in allp)
    s = 0.9 * min(width / (maxx - minx), height / (maxy - miny))
    cxo, cyo = (width - s * (maxx - minx)) / 2, (height - s * (maxy - miny)) / 2
    scr = [[((p[0] - minx) * s + cxo, height - ((p[1] - miny) * s + cyo), p[2]) for p in ps] for ps in pts]
    zbuf = [[1e9] * width for _ in range(height)]
    img = [[(40, 44, 52)] * width for _ in range(height)]
    light = (0.4, 0.8, -0.45)
    ll = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ll for c in light)
    for mi, m in enumerate(meshes):
        sp, wp = scr[mi], pts[mi]
        tint = (205, 170, 120) if m["material"] % 2 == 0 else (190, 190, 200)
        for a, b, c in m["tris"]:
            A, B, C = sp[a], sp[b], sp[c]
            # face normal in view space for shading
            u = [wp[b][k] - wp[a][k] for k in range(3)]
            v = [wp[c][k] - wp[a][k] for k in range(3)]
            nx, ny, nz = u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]
            nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            shade = 0.25 + 0.75 * abs(nx * light[0] + ny * light[1] + nz * light[2]) / nl
            colr = tuple(min(255, int(t * shade)) for t in tint)
            x0, x1 = int(max(0, min(A[0], B[0], C[0]))), int(min(width - 1, max(A[0], B[0], C[0])))
            y0, y1 = int(max(0, min(A[1], B[1], C[1]))), int(min(height - 1, max(A[1], B[1], C[1])))
            den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1])
            if abs(den) < 1e-12:
                continue
            for yy in range(y0, y1 + 1):
                row, zrow = img[yy], zbuf[yy]
                for xx in range(x0, x1 + 1):
                    w1 = ((B[1] - C[1]) * (xx - C[0]) + (C[0] - B[0]) * (yy - C[1])) / den
                    w2 = ((C[1] - A[1]) * (xx - C[0]) + (A[0] - C[0]) * (yy - C[1])) / den
                    w3 = 1 - w1 - w2
                    if w1 < 0 or w2 < 0 or w3 < 0:
                        continue
                    z = w1 * A[2] + w2 * B[2] + w3 * C[2]
                    if z < zrow[xx]:
                        zrow[xx] = z
                        row[xx] = colr
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in img)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stem", help="path without extension: <stem>.json and <stem>.render.bin")
    ap.add_argument("--lod", type=int, default=0)
    ap.add_argument("--obj")
    ap.add_argument("--png")
    ap.add_argument("--yaw", type=float, default=60.0)
    args = ap.parse_args()
    model = load_record(args.stem + ".json")
    blob = open(args.stem + ".render.bin", "rb").read()
    meshes = [decode_mesh(model, blob, i) for i in lod_meshes(model, args.lod)]
    tris = sum(len(m["tris"]) for m in meshes)
    verts = sum(len(m["pos"]) for m in meshes)
    print("%s LOD%d: %d meshes, %d vertices, %d triangles" % (model["debugName"], args.lod, len(meshes), verts, tris))
    if meshes and meshes[0]["skin"]:
        wsum = [round(sum(w) * 255) for m in meshes for _, w in m["skin"]]
        print("skin weights sum to 255 for %d of %d vertices" % (sum(1 for x in wsum if x == 255), len(wsum)))
    if args.obj:
        write_obj(model, meshes, args.obj)
    if args.png:
        write_png(meshes, args.png, yaw_deg=args.yaw)


if __name__ == "__main__":
    main()
