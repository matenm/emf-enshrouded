"""Game skeleton + skinned model -> glTF binary (.glb) for Blender, and the shared coordinate rules.

Coordinate systems (checked against the game's skeletons and models):
  game: left-handed, Y up, characters face +Z, their right hand at +X (all humanoid skeletons).
  glTF: right-handed, Y up, models face +Z, their right hand at -X.
So game <-> glTF is a mirror in X (its own inverse): positions/normals/tangents x -> -x, tangent w -> -w,
quaternions (x, y, z, w) -> (x, -y, -z, w), matrices M -> S M S with S = diag(-1, 1, 1).
Triangle order stays: game triangles are clockwise against their normals in right-handed maths, the mirror makes
them counter-clockwise as glTF wants.

Skeleton (keen::ModelHierarchyResource): jointParentIndices (65535 = none), defaultPoseJoints (local TRS; the
array has 2 padding entries), skinToJointMatrices (inverse bind; the rot fields x/y/z are COLUMNS), jointNames
(CRC32 of the lowercase name; only 'root' is known). Bone names exported here are 'jNNN' plus the attachment slot
name when an animation graph maps one (j017_Head); the importer maps bones back by the jNNN prefix.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rendermodel as rm  # noqa: E402

SLOTS = ["Root", "Head", "Chest", "Pelvis", "FootL", "FootR", "FootRL", "FootRR", "HandR", "HandL", "KneeR",
         "KneeL", "ArmL", "Block", "HandPalmR", "HandPalmL"] + ["Generic%02d" % i for i in range(20)]
NO_PARENT = 65535


# --- mirror helpers (game <-> glTF, self-inverse) ---------------------------------------------------------
def mirror_vec(v):
    return (-v[0], v[1], v[2])


def mirror_quat(q):
    x, y, z, w = q
    return (x, -y, -z, w)


def mirror_mat(m):
    """S M S for a 4x4 row-major matrix, S = diag(-1, 1, 1, 1)."""
    s = (-1.0, 1.0, 1.0, 1.0)
    return [[m[r][c] * s[r] * s[c] for c in range(4)] for r in range(4)]


def inverse_bind_rows(entry):
    """skinToJointMatrices entry -> 4x4 row-major matrix (rot fields are columns)."""
    r, p = entry["rot"], entry["pos"]
    return [[r["x"]["x"], r["y"]["x"], r["z"]["x"], p["x"]],
            [r["x"]["y"], r["y"]["y"], r["z"]["y"], p["y"]],
            [r["x"]["z"], r["y"]["z"], r["z"]["z"], p["z"]],
            [0.0, 0.0, 0.0, 1.0]]


def joint_names(hier, slot_map=None):
    names = ["j%03d" % i for i in range(hier["jointCount"])]
    if slot_map:
        used = set()
        for slot_index, bone in enumerate(slot_map):
            if slot_index == 0 and bone == 0:
                names[0] = "j000_Root"
                continue
            if 0 < bone < len(names) and slot_index < len(SLOTS) and bone not in used:
                names[bone] = "j%03d_%s" % (bone, SLOTS[slot_index])
                used.add(bone)
    return names


# --- glb writer ---------------------------------------------------------------------------------------------
class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.gltf = {"asset": {"version": "2.0", "generator": "EMF tools/models"}, "buffers": [],
                     "bufferViews": [], "accessors": [], "nodes": [], "meshes": [], "skins": [],
                     "scenes": [{"nodes": []}], "scene": 0, "materials": []}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.gltf["bufferViews"].append(view)
        return len(self.gltf["bufferViews"]) - 1

    def accessor(self, values, ctype, atype, target=None, minmax=False, normalized=False):
        fmt = {5126: "f", 5123: "H", 5125: "I", 5121: "B"}[ctype]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[atype]
        flat = [x for v in values for x in (v if width > 1 else (v,))]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        acc = {"bufferView": self._view(data, target), "componentType": ctype, "count": len(values), "type": atype}
        if normalized:
            acc["normalized"] = True
        if minmax:
            acc["min"] = [min(v[k] for v in values) for k in range(width)]
            acc["max"] = [max(v[k] for v in values) for k in range(width)]
        self.gltf["accessors"].append(acc)
        return len(self.gltf["accessors"]) - 1

    def write(self, path):
        self.gltf["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.gltf, separators=(",", ":")).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        binp = bytes(self.bin) + b"\0" * ((4 - len(self.bin) % 4) % 4)
        total = 12 + 8 + len(js) + 8 + len(binp)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(binp), 0x004E4942) + binp)


def export(model, blob, hier, path, lod=0, slot_map=None, extras=None):
    """Write the skeleton (armature) and the model's LOD meshes, skinned, as .glb."""
    g = GlbBuilder()
    names = joint_names(hier, slot_map)
    n = hier["jointCount"]
    parents = hier["jointParentIndices"][:n]
    pose = hier["defaultPoseJoints"][:n]
    node_of_joint = []
    for i in range(n):
        j = pose[i]
        q = (j["orientation"]["x"], j["orientation"]["y"], j["orientation"]["z"], j["orientation"]["w"])
        t = (j["position"]["x"], j["position"]["y"], j["position"]["z"])
        s = (j["scale"]["x"], j["scale"]["y"], j["scale"]["z"])
        node = {"name": names[i], "rotation": list(mirror_quat(q)), "translation": list(mirror_vec(t))}
        if any(abs(c - 1.0) > 1e-5 for c in s):
            node["scale"] = list(s)
        g.gltf["nodes"].append(node)
        node_of_joint.append(len(g.gltf["nodes"]) - 1)
    roots = []
    for i, p in enumerate(parents):
        if p == NO_PARENT:
            roots.append(node_of_joint[i])
        else:
            g.gltf["nodes"][node_of_joint[p]].setdefault("children", []).append(node_of_joint[i])
    ibm = []
    for e in hier["skinToJointMatrices"][:n]:
        m = mirror_mat(inverse_bind_rows(e))
        ibm.append(tuple(m[r][c] for c in range(4) for r in range(4)))  # column-major
    skin = {"name": "game_skeleton", "joints": node_of_joint, "skeleton": roots[0],
            "inverseBindMatrices": g.accessor(ibm, 5126, "MAT4")}
    g.gltf["skins"].append(skin)
    armature = {"name": "armature", "children": roots}
    g.gltf["nodes"].append(armature)
    g.gltf["scenes"][0]["nodes"].append(len(g.gltf["nodes"]) - 1)

    mats = {}
    for mi in rm.lod_meshes(model, lod):
        d = rm.decode_mesh(model, blob, mi)
        if d["material"] not in mats:
            g.gltf["materials"].append({"name": "game_material_%d" % d["material"],
                                        "pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.62, 0.52, 1.0],
                                                                 "metallicFactor": 0.0, "roughnessFactor": 0.8}})
            mats[d["material"]] = len(g.gltf["materials"]) - 1
        attrs = {
            "POSITION": g.accessor([mirror_vec(p) for p in d["pos"]], 5126, "VEC3", 34962, minmax=True),
            "NORMAL": g.accessor([mirror_vec(v) for v in d["nrm"]], 5126, "VEC3", 34962),
            "TANGENT": g.accessor([(-t[0], t[1], t[2], -t[3]) for t in d["tan"]], 5126, "VEC4", 34962),
            "TEXCOORD_0": g.accessor(d["uv"], 5126, "VEC2", 34962),
        }
        if d["skin"]:
            attrs["JOINTS_0"] = g.accessor([tuple(j) for j, _ in d["skin"]], 5123, "VEC4", 34962)
            attrs["WEIGHTS_0"] = g.accessor([tuple(w) for _, w in d["skin"]], 5126, "VEC4", 34962)
        idx = [i for tri in d["tris"] for i in tri]
        prim = {"attributes": attrs, "indices": g.accessor(idx, 5125, "SCALAR", 34963), "material": mats[d["material"]]}
        g.gltf["meshes"].append({"name": "%s_mesh%d" % (model["debugName"], mi), "primitives": [prim]})
        mnode = {"name": "%s_mesh%d" % (model["debugName"], mi), "mesh": len(g.gltf["meshes"]) - 1}
        if d["skin"]:
            mnode["skin"] = 0
        g.gltf["nodes"].append(mnode)
        g.gltf["scenes"][0]["nodes"].append(len(g.gltf["nodes"]) - 1)
    g.gltf["extras"] = dict(extras or {}, jointNameHashes=[h & 0xFFFFFFFF for h in hier["jointNames"][:n]],
                            coordinateNote="game space is left-handed; this file is mirrored in X")
    g.write(path)
    return names


if __name__ == "__main__":
    import argparse
    import csv

    ap = argparse.ArgumentParser(description="Export a game model + its skeleton to .glb")
    ap.add_argument("stem", help="<stem>.json, <stem>.render.bin")
    ap.add_argument("--hierarchy", required=True, help="hierarchy JSON (model_survey hierarchies/<guid>.json)")
    ap.add_argument("--graph-slots", help="graph_slots.csv to name bones")
    ap.add_argument("--out", required=True)
    ap.add_argument("--lod", type=int, default=0)
    a = ap.parse_args()
    model = rm.load_record(a.stem + ".json")
    blob = open(a.stem + ".render.bin", "rb").read()
    hier = rm.load_record(a.hierarchy)
    slot_map = None
    if a.graph_slots:
        for row in csv.DictReader(open(a.graph_slots, encoding="utf-8")):
            if row["hierarchy"] == model["hierarchy"]:
                slot_map = [int(x) for x in row["slotBoneIndexMapping"].split()]
                break
    names = export(model, blob, hier, a.out, a.lod, slot_map,
                   {"model": model["debugName"], "hierarchy": model["hierarchy"]})
    print("wrote", a.out, "joints", len(names), "named", sum(1 for x in names if "_" in x))
