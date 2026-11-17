"""A creature on a skeleton of its own (make_mod's "new_skeleton"): a skinned, animated glb becomes a game skeleton
(new_skeleton.py), the model on it, its animations game clips (gltf_clips.py), and a copy of a game creature's
animation graph plays them.

The pack names, per creature:
  scale          the size: the file's metres times this
  skeleton_guid  the skeleton's GUID (fixed forever; the clips' GUIDs follow from it and their names)
  graph, graph_guid   the game's animation graph and its copy's GUID
  clips          our clips: {name: {"animation": the glb's animation, "loop": true for a looping clip,
                 "speed": faster above 1, "in_place": the joint that carries a walk forward when the animation
                 travels (root motion: its travel along the ground is taken out, gltf_clips.clip)}}, or {name: {"made": {"from": an animation (else the rest pose), "at":
                 seconds into it, or "base": a timeline (below) whose poses the turns go on top of, as long as the
                 keys, "keys": made_clips.py's key turns, "loop": ...}}} (a death or flinch the file lacks), or
                 {name: {"additive_zero": true}}: an additive clip that adds nothing (every joint unturned,
                 unmoved, scale 0: an additive layer adds scales, as the game's offset clips show), for the graph's
                 additive layers whose game clips are offsets (the wolf's leaning), or {name: {"offset": {joint:
                 [axis, degrees]}}}: an offset that turns those joints (made_clips.offset; the game wolf's lean turns
                 its neck and head about 50 degrees into a turn), or {name: {"snake": {"head": the first joint of
                 a snake's spine chain, "keys": snake_clips.py's keys, "length": seconds, "travel": waves along the
                 body over the clip, "loop": ...}}}: a snake's clip from its body curve (slither, rear, strike,
                 roll), or {name: {"timeline": [[at, animation, time], ...]}}: a clip that times the glb's
                 animations (gltf_clips.timeline: a swing stretched to land when the game's attack hits, two swings
                 joined into one combo)
  model_glb     optional: the model from this glb instead (a copy on the skeleton, new_skeleton.write_glb, then
                 retextured; its bones named jNNN_<name>)
  roles          {game clip GUID: our clip name}: which of our clips plays where the graph plays that game clip
  default_role   our clip for every other game clip the graph plays: every clip reference of the copy is ours (a
                 game clip carries the game skeleton's hash and cannot play on ours). A node whose clips are offsets
                 (animationContentAdditive) plays an offset clip of ours: one of the pack's additive_zero clips, or
                 the built-in zero offset (_additive_zero) where its role names a whole pose
  slots          {slot: our joint name}: the graph's attachment slots (gamegltf.SLOTS: Root, Head, Chest, Pelvis,
                 FootL, ...); a slot not named points at the root
  uneven_scale   optional, true: the file is scaled unevenly above its joints (an FBX export's 1.2 x 1.2 x 1.1):
                 fold that into the joints' positions, each joint taking the nearest rotation (new_skeleton.decompose)
  yaw            optional: degrees the file is turned about the vertical first (glTF's, counter-clockwise seen from
                 above): a creature lying along +x faces the game's forward with -90
  widen          optional: the model this much wider across the body (widen, below); the skeleton and the clips
                 stay as they are
"""
import os
import re
import sys
import tempfile
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamegltf  # noqa: E402
import gltf_clips  # noqa: E402
import gltf_in  # noqa: E402
import joint_animation  # noqa: E402
import made_clips  # noqa: E402
import new_skeleton  # noqa: E402
import snake_clips  # noqa: E402

GUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
ZERO = "_additive_zero"  # the built-in offset that adds nothing
ADDITIVE_FRAMES = 9  # as long as the game wolf's leaning offsets
KEYS = ("scale", "skeleton_guid", "graph", "graph_guid", "clips", "roles", "default_role", "slots")


def clip_guid(skeleton_guid, name):
    return str(uuid.uuid5(uuid.UUID(skeleton_guid), "clip:" + name))


def check(cfg, creature):
    """The pack's mistakes, as SystemExit: missing keys, a role or the default naming a clip the pack has not."""
    missing = [k for k in KEYS if k not in cfg or cfg[k] in (None, "")]
    if missing:
        raise SystemExit("%s: new_skeleton needs %s" % (creature, ", ".join(missing)))
    clips = cfg["clips"]
    if not cfg["roles"]:
        raise SystemExit("%s: new_skeleton roles is empty: name the graph's game clips (their GUIDs)" % creature)
    for game in cfg["roles"]:
        if not GUID.match(game.lower()):
            raise SystemExit("%s: new_skeleton role %r is not a game clip GUID" % (creature, game))
    for name in clips:
        if name.startswith("_"):
            raise SystemExit("%s: clip names starting with _ are the tools' own (%s)" % (creature, name))
    for game, ours in list(cfg["roles"].items()) + [("default_role", cfg["default_role"])]:
        if ours not in clips:
            raise SystemExit("%s: new_skeleton role %s names the clip %r, which is not in clips (%s)"
                             % (creature, game, ours, ", ".join(sorted(clips))))
    w = cfg.get("widen", 1.0)
    if isinstance(w, bool) or not isinstance(w, (int, float)) or w <= 0:
        raise SystemExit("%s: new_skeleton widen is %r: a number above 0 (1.2: a fifth wider)" % (creature, w))


def roles(cfg):
    """({game clip GUID: our clip GUID}, the default's GUID)."""
    g = cfg["skeleton_guid"]
    return ({game.lower(): clip_guid(g, ours) for game, ours in cfg["roles"].items()},
            clip_guid(g, cfg["default_role"]))


def offsets(cfg):
    """{GUID: True} of our offset clips: the pack's additive_zero clips and the built-in zero offset."""
    g = cfg["skeleton_guid"]
    names = [n for n, spec in cfg["clips"].items() if spec.get("additive_zero") or spec.get("offset")] + [ZERO]
    return {clip_guid(g, n): True for n in names}


def zero_clip(sk, hier):
    """An offset that adds nothing: every joint unturned, unmoved, scale 0 (an additive layer adds scales, as the
    game's offset clips show)."""
    zero = [((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))] * len(sk["rest"])
    return joint_animation.encode([zero] * (ADDITIVE_FRAMES + 1), sk["rest"],
                                  hierarchy_hash=new_skeleton.clip_hash(hier))


def build(cfg, glb_path, creature="creature"):
    """{sk, hierarchy, clips: [{name, guid, clip}] by name, zero: the built-in zero offset (the same shape),
    slots: 36 joint indices}."""
    check(cfg, creature)
    gltf, binc = gltf_in.read_glb(glb_path)
    sk = new_skeleton.build(gltf, binc, scale=float(cfg["scale"]), uneven=bool(cfg.get("uneven_scale")),
                            yaw=float(cfg.get("yaw", 0.0)))
    hier = new_skeleton.hierarchy(sk, cfg["skeleton_guid"])
    slots = [0] * len(gamegltf.SLOTS)
    for slot, joint in cfg["slots"].items():
        if slot not in gamegltf.SLOTS:
            raise SystemExit("%s: no slot %r (the slots: %s)" % (creature, slot, ", ".join(gamegltf.SLOTS)))
        if joint not in sk["names"]:
            raise SystemExit("%s: slot %s names %r, which is not a joint of the skeleton" % (creature, slot, joint))
        slots[gamegltf.SLOTS.index(slot)] = sk["names"].index(joint)
    have = gltf_clips.names(gltf)
    out = []
    for name in sorted(cfg["clips"]):
        spec = cfg["clips"][name]
        if spec.get("additive_zero"):
            out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": zero_clip(sk, hier)})
            continue
        if spec.get("offset"):
            try:
                clip = made_clips.offset(sk, spec["offset"], hierarchy_hash=new_skeleton.clip_hash(hier))
            except ValueError as e:
                raise SystemExit("%s: clip %s: %s" % (creature, name, e))
            out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": clip})
            continue
        if spec.get("snake"):
            sn = spec["snake"]
            try:
                clip = snake_clips.clip(sk, sn.get("head", ""), sn.get("keys", []), float(sn.get("length", 1.0)),
                                        travel=float(sn.get("travel", 0.0)), loop=bool(sn.get("loop")),
                                        hierarchy_hash=new_skeleton.clip_hash(hier))
            except (KeyError, ValueError) as e:
                raise SystemExit("%s: clip %s: %s" % (creature, name, e.args[0] if e.args else e))
            out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": clip})
            continue
        if spec.get("made"):
            made = spec["made"]
            if made.get("from") and made["from"] not in have:
                raise SystemExit("%s: clip %s is made from %r, which the glb does not have" % (creature, name,
                                                                                            made["from"]))
            missing = [k[1] for k in made.get("base", []) if len(k) > 1 and k[1] not in have]
            if missing:
                raise SystemExit("%s: clip %s is made on %r, which the glb does not have" % (creature, name,
                                                                                            missing[0]))
            try:
                bases = gltf_clips.timeline_poses(gltf, binc, sk, made["base"]) if made.get("base") else None
                base = (gltf_clips.pose_at(gltf, binc, sk, made["from"], float(made.get("at", 0.0)))
                        if made.get("from") else sk["rest"])
                clip = made_clips.make(sk, base, made.get("keys", []), loop=bool(made.get("loop")),
                                       hierarchy_hash=new_skeleton.clip_hash(hier), bases=bases)
            except ValueError as e:
                raise SystemExit("%s: clip %s: %s" % (creature, name, e))
            out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": clip})
            continue
        if "timeline" in spec:
            keys = spec["timeline"]
            missing = [k[1] for k in keys if len(k) > 1 and k[1] not in have]
            if missing:
                raise SystemExit("%s: clip %s plays %r, which the glb does not have (%s)"
                                 % (creature, name, missing[0], ", ".join(have)))
            try:
                clip = gltf_clips.timeline(gltf, binc, sk, keys, new_skeleton.clip_hash(hier))
            except ValueError as e:
                raise SystemExit("%s: clip %s: %s" % (creature, name, e))
            out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": clip})
            continue
        if spec.get("animation") not in have:
            raise SystemExit("%s: clip %s plays %r, which the glb does not have (%s)"
                             % (creature, name, spec.get("animation"), ", ".join(have)))
        try:
            clip = gltf_clips.clip(gltf, binc, sk, spec["animation"], new_skeleton.clip_hash(hier),
                                   loop=bool(spec.get("loop")), speed=float(spec.get("speed", 1.0)),
                                   in_place=spec.get("in_place"))
        except (KeyError, ValueError) as e:
            raise SystemExit("%s: clip %s: %s" % (creature, name, e.args[0] if e.args else e))
        out.append({"name": name, "guid": clip_guid(cfg["skeleton_guid"], name), "clip": clip})
    zero = {"name": ZERO, "guid": clip_guid(cfg["skeleton_guid"], ZERO), "clip": zero_clip(sk, hier)}
    return {"sk": sk, "hierarchy": hier, "clips": out, "zero": zero, "slots": slots}


def widen(groups, hier, factor):
    """The groups (gltf_in.load_groups') with every vertex moved across the body (x) away from the point its joints
    stand at, weighted as its skin is, by factor: a body on the spine gets broader, a leg on its own joints thicker
    without moving off them. Up and forward stay; normals and tangents follow the stretch. New groups."""
    xs = [gltf_in.mat_inv_affine(gamegltf.inverse_bind_rows(e))[0][3]
          for e in hier["skinToJointMatrices"][:hier["jointCount"]]]

    def unit(v):
        n = sum(c * c for c in v) ** 0.5 or 1.0
        return tuple(c / n for c in v)

    out = []
    for g in groups:
        g = dict(g)
        pos = []
        for p, (js, ws) in zip(g["pos"], g["skin"]):
            mid = sum(w * xs[j] for j, w in zip(js, ws)) / (sum(ws) or 1.0)
            pos.append((mid + (p[0] - mid) * factor, p[1], p[2]))
        g["pos"] = pos
        g["nrm"] = [unit((n[0] / factor, n[1], n[2])) for n in g["nrm"]]
        g["tan"] = [unit((t[0] * factor, t[1], t[2])) + (t[3],) for t in g["tan"]]
        out.append(g)
    return out


def model_groups(glb_path, built, material_count=None, warn=print, model_glb=None, widen_by=1.0):
    """The model on the skeleton, as gltf_in.load_groups gives it (game space, our joint indices): from model_glb
    when given (a copy on the skeleton), else from the glb; widened by widen_by (widen)."""
    if model_glb:
        groups, report = gltf_in.load_groups(model_glb, built["hierarchy"], material_count=material_count, warn=warn)
    else:
        gltf, binc = gltf_in.read_glb(glb_path)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "model.glb")
            new_skeleton.write_glb(gltf, binc, built["sk"], path)
            groups, report = gltf_in.load_groups(path, built["hierarchy"], material_count=material_count, warn=warn)
    if widen_by != 1.0:
        groups = widen(groups, built["hierarchy"], widen_by)
    return groups, report
