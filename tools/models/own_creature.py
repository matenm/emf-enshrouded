"""A creature on its own proportions, for make_mod.py: a copy of the game skeleton fitted to the creature, and every
clip of the game creature's animation graph retargeted to it.

The creature's pack entry has "own_proportions":
  sidecar          the fitter's sidecar (blender/fit_to_game_skeleton.py --own-proportions): every game joint moved
                   into the creature's shape
  skeleton_guid    the skeleton copy's GUID (fixed forever, like the model's)
  reference_clip   a game clip whose first frame is the game creature standing (its idle): motions are taken
                   relative to it and put onto the creature standing in its own shape; or "rest": the game skeleton's
                   rest pose, for a creature made in that pose (a model from pictures of the game body)
  clips            a glob of the game clips to retarget (EML dumps of keen::JointAnimation, named <owner>_<guid>.json):
                   every clip the graph can play
  graph            the game creature's animation graph GUID; graph_guid: its copy's GUID

The skeleton copy keeps the game skeleton's joints and names (so its hash, and the clips' link to it, stay) and every
bone's frame (the rest rotations); only the joints move, to where the creature stands in its own shape, and its
inverse binds are that pose's. A clip frame becomes: each joint turned in the world as the game creature's turned away
from its standing pose (own_skeleton.retarget), which does not depend on the bones' frames. The frames stay the
game's because the game graph's own nodes (foot IK, leaning, aiming) turn bones about their axes.
"""
import glob
import json
import os
import re
import uuid

import joint_animation as ja
import own_skeleton as osk

GUID = re.compile(r"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$")
JOINT = re.compile(r"^j(\d+)")


def build(cfg, hier):
    """{"bind": rest pose, "skeleton": {source, guid, rest, skinToJointMatrices}, "clips": [{source, guid, clip}]}.
    cfg: the pack entry's own_proportions (paths already resolved); hier: the game skeleton as EML shows it."""
    rest, parents = osk.rest_of(hier)
    n = len(rest)
    with open(cfg["sidecar"], encoding="utf-8") as f:
        joints = json.load(f)["joints"]
    by_index = {}
    for j in joints:  # joint k is "jNNN..." (gamegltf.py's names), whatever the order
        m = JOINT.match(j["name"])
        if not m:
            raise SystemExit("own_proportions: the sidecar joint %r is not named jNNN" % j["name"])
        by_index[int(m.group(1))] = j
    if sorted(by_index) != list(range(n)):
        raise SystemExit("own_proportions: the sidecar's joints are not the skeleton's %d" % n)
    joints = [by_index[k] for k in range(n)]
    want = ja.hierarchy_hash(hier["jointNames"][:n])

    def load_clip(path):
        with open(path, encoding="utf-8") as f:
            clip = json.load(f)
        if clip["jointCount"] != n or clip["hierarchyHash"] & 0xFFFFFFFFFFFFFFFF != want:
            raise SystemExit("own_proportions: %s is a clip of another skeleton" % path)
        return clip

    # only the joints move: every bone keeps the game skeleton's frame, so the game graph's own nodes (foot IK,
    # leaning, aiming) turn bones about the axes they expect; the motion does not depend on the frames (retarget
    # works in the world)
    bind = osk.rest_from_world(rest, parents, [osk.blender_to_game(j["own"]) for j in joints])
    ref = rest if cfg["reference_clip"] == "rest" else ja.sample(load_clip(cfg["reference_clip"]), 0, rest)
    clips = []
    for path in sorted(glob.glob(cfg["clips"])):
        clip = load_clip(path)
        m = GUID.search(os.path.splitext(os.path.basename(path))[0])
        if not m:
            raise SystemExit("own_proportions: %s: the file name does not end in the clip's GUID" % path)
        source = m.group(1)
        frames = clip["frameCount"]
        poses = [ja.sample(clip, f, rest) for f in range(frames)]
        poses.append(ja.sample(clip, frames, rest) if clip["flags"] == 1 else ja.sample_end(clip, rest))
        poses = [osk.retarget(p, ref, bind, parents) for p in poses]
        enc = ja.encode(poses, bind, fps=clip["fps"], loop=clip["flags"] == 1, hierarchy_hash=clip["hierarchyHash"])
        clips.append({"source": source, "guid": str(uuid.uuid5(uuid.UUID(cfg["skeleton_guid"]), source)), "clip": enc})
    if not clips:
        raise SystemExit("own_proportions: no clips match %s" % cfg["clips"])
    skeleton = {"source": hier["hierarchyId"], "guid": cfg["skeleton_guid"], "rest": bind,
                "skinToJointMatrices": osk.inverse_binds(bind, parents)}
    return {"bind": bind, "skeleton": skeleton, "clips": clips}
