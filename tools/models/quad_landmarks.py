"""Landmarks of a four-legged body and the height map that fits one body onto another, for
blender/fit_to_game_skeleton.py. Points have x, y, z (Blender after the glTF import: forward is -y, up is +z, width x).
Plain Python: tested without Blender (tests/tools/test_quad_landmarks.py)."""

LOW = 0.2      # the legs' lower parts: below this share of the height above the lowest point (the belly stays above)
FOOT = 0.06    # a foot: within this share of the height above its cluster's lowest point
GAP = 0.05     # front and hind legs: clusters of low points apart by more than this share of the body's length


def _mean(values):
    return sum(values) / len(values)


def landmarks(points):
    """{ground, ground_front, ground_back, nose, front, back, top, width, belly} of a four-legged body.

    front / back: the y of the front and the hind feet. They are found as clusters of low points along y: the first
    one, and the biggest of the others (two legs are more than a tail's tip), so a rest pose that holds one pair of
    feet lower than the other (the game sabretooth's front paws are 0.35 m below its hind ones) or a tail that droops
    behind as low as the feet (some big cats') does not confuse them.
    ground_front / ground_back: each pair's lowest point; ground: the lowest of all. top and width: within the band
    between the feet; belly: the lowest point of the middle half of that band."""
    zmin = min(p.z for p in points)
    zmax = max(p.z for p in points)
    h = zmax - zmin
    ymin = min(p.y for p in points)
    length = max(p.y for p in points) - ymin
    low = sorted((p for p in points if p.z < zmin + LOW * h), key=lambda p: p.y)
    clusters = [[low[0]]]
    for p in low[1:]:
        if p.y - clusters[-1][-1].y > GAP * length:
            clusters.append([p])
        else:
            clusters[-1].append(p)
    if len(clusters) > 1:
        front_c = clusters[0]
        back_c = max(clusters[1:], key=len)
    else:  # the legs meet low down: split the one cluster in the middle
        mid = (low[0].y + low[-1].y) / 2
        front_c = [p for p in low if p.y < mid] or low
        back_c = [p for p in low if p.y >= mid] or low

    def foot(cluster):
        lo = min(p.z for p in cluster)
        return _mean([p.y for p in cluster if p.z < lo + FOOT * h]), lo

    yf, gf = foot(front_c)
    yb, gb = foot(back_c)
    band = [p for p in points if yf <= p.y <= yb] or points
    quarter = (yb - yf) / 4
    inner = [p.z for p in points if yf + quarter <= p.y <= yb - quarter]
    top = max(p.z for p in band)
    return {"ground": zmin, "ground_front": gf, "ground_back": gb, "nose": ymin, "front": yf, "back": yb,
            "top": top, "width": max(abs(p.x) for p in band), "belly": min(inner) if inner else (top + zmin) / 2}


def fit_z(z, y, c, g):
    """The height on the game body `g` of a creature point at height z (landmarks `c`) and at the game's y.

    The creature's feet go to the game's higher pair of feet and its top to the game's top (a uniform scale). Below the
    game's belly the legs then stretch, evenly down to the feet, so that the other pair reaches its own ground: the
    front pair at the front feet's y, the hind pair at the hind feet's y, blended in between."""
    base = max(g["ground_front"], g["ground_back"])
    sz = (g["top"] - base) / ((c["top"] - c["ground"]) or 1e-6)
    zz = base + (z - c["ground"]) * sz
    belly = g["belly"]
    if zz >= belly or belly <= base:
        return zz
    kf = (belly - g["ground_front"]) / (belly - base)
    kb = (belly - g["ground_back"]) / (belly - base)
    t = min(1.0, max(0.0, (y - g["front"]) / ((g["back"] - g["front"]) or 1e-6)))
    return belly - (belly - zz) * (kf + (kb - kf) * t)


def warp_y(y, c, g, keep_head=False):
    """Along the body: piecewise linear through (nose, front feet, back feet); beyond the back feet the front->back
    slope goes on. keep_head: the head keeps the body's slope too (the creature's own head length, not the game's)."""
    if y <= c["front"] and not keep_head:
        t = (y - c["nose"]) / ((c["front"] - c["nose"]) or 1e-6)
        return g["nose"] + t * (g["front"] - g["nose"])
    slope = (g["back"] - g["front"]) / ((c["back"] - c["front"]) or 1e-6)
    return g["front"] + (y - c["front"]) * slope


def unwarp_y(y, c, g, keep_head=False):
    """The creature's y of a game-body y (the inverse of warp_y)."""
    if y <= g["front"] and not keep_head:
        t = (y - g["nose"]) / ((g["front"] - g["nose"]) or 1e-6)
        return c["nose"] + t * (c["front"] - c["nose"])
    slope = (g["back"] - g["front"]) / ((c["back"] - c["front"]) or 1e-6)
    return c["front"] + (y - g["front"]) / slope


def unfit_z(z, y, c, g):
    """The creature's height of a game-body height z at the game's y (the inverse of fit_z)."""
    base = max(g["ground_front"], g["ground_back"])
    sz = (g["top"] - base) / ((c["top"] - c["ground"]) or 1e-6)
    belly = g["belly"]
    zz = z
    if z < belly and belly > base:
        kf = (belly - g["ground_front"]) / (belly - base)
        kb = (belly - g["ground_back"]) / (belly - base)
        t = min(1.0, max(0.0, (y - g["front"]) / ((g["back"] - g["front"]) or 1e-6)))
        zz = belly - (belly - z) / (kf + (kb - kf) * t)
    return c["ground"] + (zz - base) / sz


def warp(p, c, g, sx, keep_head=False):
    """A creature point on the game body (what the fitter does to every vertex)."""
    y = warp_y(p.y, c, g, keep_head)
    return p.x * sx, y, fit_z(p.z, y, c, g)


def unwarp(p, c, g, sx, keep_head=False):
    """A game-body point in the creature's own shape (the inverse of warp)."""
    return p.x / sx, unwarp_y(p.y, c, g, keep_head), unfit_z(p.z, p.y, c, g)


def own_space(p, c, g, size=1.0):
    """A point of the creature's own shape at the game body's size: a uniform scale (the fit's height scale times
    size) that puts the creature's front feet on the ground where the game's front feet stand (at the higher of the
    game's two grounds)."""
    base = max(g["ground_front"], g["ground_back"])
    k = (g["top"] - base) / ((c["top"] - c["ground"]) or 1e-6) * size
    return p.x * k, g["front"] + (p.y - c["front"]) * k, base + (p.z - c["ground"]) * k


def width_scale(c, g, mode="fit"):
    """The x scale for a creature with landmarks `c` on the game body `g`.

    "fit": to the game body's width (the creature's legs sit on the game's). "keep": the creature's own proportions,
    the mean of its length scale (between the feet) and its height scale, for a creature that the fit stretches much
    more in length and height than in width (fitted to the game body's width, it is thinner than designed).
    A number: that scale."""
    if mode == "fit":
        return g["width"] / (c["width"] or 1e-6)
    if mode == "keep":
        length = (g["back"] - g["front"]) / ((c["back"] - c["front"]) or 1e-6)
        height = (g["top"] - max(g["ground_front"], g["ground_back"])) / ((c["top"] - c["ground"]) or 1e-6)
        return (length + height) / 2
    return float(mode)


STEP = 0.05         # slices along the body (y), and cells within a slice (x, z), in metres
THIN_FILL = 0.3     # a tail slice fills less than this share of the body's typical cross-section ...
THIN_REACH = 0.35   # ... or reaches less than this share of the body's width to the side
HEAD_SHARE = 0.05   # a bone under the head is kept when the game animal puts this share of its head's weight on it


def tail_slices(points, g):
    """The slices (round(y / STEP)) behind the hind feet (landmarks `g` of the fitted creature) where the creature is
    thin: its tail. A slice is thin when its points fill little of the cross-section (a tail curling out to the side
    reaches far but fills little) or reach little to the side (a tail hanging right behind the rump)."""
    cells, reach = {}, {}
    for p in points:
        k = round(p.y / STEP)
        cells.setdefault(k, set()).add((round(p.x / STEP), round(p.z / STEP)))
        reach[k] = max(reach.get(k, 0.0), abs(p.x))
    fill = {k: len(c) for k, c in cells.items()}
    body = sorted(fill[k] for k in fill if g["front"] <= k * STEP <= g["back"])
    ref = body[len(body) // 2] if body else 1
    xs = sorted(abs(p.x) for p in points if g["front"] <= p.y <= g["back"])
    width = xs[int(0.9 * len(xs))] if xs else 1.0
    start = g["back"] - 0.05 * (g["back"] - g["front"])
    return {k for k in fill if k * STEP > start and (fill[k] < THIN_FILL * ref or reach[k] < THIN_REACH * width)}


def _spine(parents, head):
    spine, b = set(), head
    while b is not None:
        spine.add(b)
        b = parents.get(b)
    return spine


def leg_chains(parents, z, head):
    """The four legs: the four branches off the spine (outside the head) that reach lowest in the rest pose. Game
    skeletons do not name every leg (the boar's leg bones have numbers only), but feet are always far lower than a
    tail or a helper bone. `parents`: bone -> parent (None at the root); `z`: bone -> its rest-pose height. Returns
    [(root, the spine bone it hangs from, every bone in the leg)]. Raises ValueError when fewer than four branches
    leave the spine."""
    children = {}
    for b, p in parents.items():
        children.setdefault(p, []).append(b)

    def subtree(b):
        out = {b}
        for c in children.get(b, []):
            out |= subtree(c)
        return out

    spine = _spine(parents, head)
    branches = [(c, s, subtree(c)) for s in spine - {head} for c in children.get(s, []) if c not in spine]
    if len(branches) < 4:
        raise ValueError("%d branches leave the spine, a four-legged skeleton has at least 4" % len(branches))
    return sorted(branches, key=lambda br: min(z[x] for x in br[2]))[:4]


def main_bones(parents, z, head):
    """The bones that keep their copied weights: the spine (`head` and every bone above it) and the four legs
    (leg_chains), with every bone in them. Raises ValueError when fewer than four branches leave the spine."""
    return _spine(parents, head).union(*(bones for _, _, bones in leg_chains(parents, z, head)))


SIDE_FADE = 0.5  # --sides: a leg's weight fades out over this share of the way from the centre line to its root


def side_weights(weights, xs, legs):
    """A vertex hangs on a leg only on that leg's side of the centre line, and the leg's weight fades out towards the
    line over SIDE_FADE of the way from it to the leg's root; what a leg loses goes to the bone the leg hangs from.
    Thin parts between two legs (a skeleton's pelvis, a web of sinew) then follow the body instead of tearing apart when
    the legs swing opposite ways. `weights`: per vertex {bone: weight}; `xs`: per vertex its x; `legs`: [(the leg's
    bones, x of its root, x of the bone it hangs from (the centre line), that bone)]; a leg whose root is on the centre
    line has no side and stays as it is. Returns new per-vertex dicts."""
    sided = []
    for bones, root_x, centre_x, parent in legs:
        span = (root_x - centre_x) * SIDE_FADE
        if abs(span) > 1e-9:
            sided.append((bones, centre_x, span, parent))
    out = []
    for ws, x in zip(weights, xs):
        new = dict(ws)
        for bones, centre_x, span, parent in sided:
            keep = min(1.0, max(0.0, (x - centre_x) / span))
            if keep >= 1.0:
                continue
            for b in bones & set(new):
                moved = new[b] * (1.0 - keep)
                if keep > 0.0:
                    new[b] -= moved
                else:
                    del new[b]
                new[parent] = new.get(parent, 0.0) + moved
        out.append(new)
    return out


def kept_head_bones(use, total):
    """The bones under the head the game animal itself uses on its head (`use`: bone -> weight there; `total`: all
    weight there): its jaw, not a crest or ear tips that belong to other meshes."""
    return {b for b, w in use.items() if w >= HEAD_SHARE * total}


def tail_distances(points, edges, in_tail, weld=1e-5):
    """Per tail vertex, its distance along the mesh's edges (within the tail) from where the tail leaves the body
    (tail vertices joined to a body vertex start at 0). A curled or hooked tail is measured along its length, not as
    the crow flies. `edges`: vertex index pairs; `in_tail`: per vertex. Vertices at the same place (within `weld`:
    the copies a mesh with its own UVs has on every UV seam) count as joined; a tail vertex the edges still do not
    reach (a loose piece) takes the distance of the nearest one they do, plus the gap."""
    import heapq
    near = {}
    for a, b in edges:
        near.setdefault(a, []).append(b)
        near.setdefault(b, []).append(a)
    cells = {}
    for v, p in enumerate(points):
        cells.setdefault((round(p.x / weld), round(p.y / weld), round(p.z / weld)), []).append(v)
    for same in cells.values():
        for a in same[1:]:
            near.setdefault(a, []).append(same[0])
            near.setdefault(same[0], []).append(a)
    dist = {}
    heap = []
    for v, tail in enumerate(in_tail):
        if tail and any(not in_tail[u] for u in near.get(v, ())):
            heap.append((0.0, v))
    heapq.heapify(heap)
    while heap:
        d, v = heapq.heappop(heap)
        if v in dist:
            continue
        dist[v] = d
        for u in near.get(v, ()):
            if in_tail[u] and u not in dist:
                p, q = points[v], points[u]
                step = ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5
                heapq.heappush(heap, (d + step, u))
    reached = list(dist)
    if reached:
        for v, tail in enumerate(in_tail):
            if tail and v not in dist:
                p = points[v]
                gap, u = min((((p.x - points[u].x) ** 2 + (p.y - points[u].y) ** 2 + (p.z - points[u].z) ** 2) ** 0.5, u)
                             for u in reached)
                dist[v] = dist[u] + gap
    return dist


def tail_curve(points, dist, n):
    """The tail's centre line: n + 1 points from where it leaves the body to its tip, each the centroid of the tail's
    vertices at that distance along it (within half a step)."""
    top = max(dist.values()) or 1.0
    half = top / (2 * n)
    curve = []
    for i in range(n + 1):
        t = top * i / n
        near = [points[v] for v, d in dist.items() if abs(d - t) <= half]
        if near:
            curve.append(tuple(sum(getattr(p, a) for p in near) / len(near) for a in "xyz"))
    return curve


def along(curve, fractions):
    """Points at the given fractions (0..1) of a polyline's length."""
    seg = [sum((a - b) ** 2 for a, b in zip(p, q)) ** 0.5 for p, q in zip(curve, curve[1:])]
    total = sum(seg) or 1.0
    out = []
    for f in fractions:
        left = min(max(f, 0.0), 1.0) * total
        for i, s in enumerate(seg):
            if left <= s or i == len(seg) - 1:
                t = min(left / s, 1.0) if s > 0 else 0.0
                out.append(tuple(a + (b - a) * t for a, b in zip(curve[i], curve[i + 1])))
                break
            left -= s
    return out


def tail_chain_weights(dist, fractions, chain, parent=None, ramp=0.0):
    """Per tail vertex, its weights on a chain of tail bones placed at `fractions` (0..1) of the tail's length: shared
    between the two bones around its own fraction (its distance along the tail over the tail's length). With
    `parent` (the bone the tail hangs from, the hips), the first `ramp` of the tail blends from it into the chain, so
    the rump and the tail's root do not tear apart when the tail turns."""
    top = max(dist.values()) or 1.0
    out = {}
    for v, d in dist.items():
        s = d / top
        if s >= fractions[-1]:
            w = {chain[-1]: 1.0}
        else:
            k = max(i for i, f in enumerate(fractions) if f <= s) if s > fractions[0] else 0
            span = fractions[k + 1] - fractions[k]
            t = (s - fractions[k]) / span if span > 0 else 0.0
            w = {chain[k]: 1.0 - t, chain[k + 1]: t}
        if parent is not None and s < ramp:
            share = 1.0 - s / ramp
            w = {b: x * (1.0 - share) for b, x in w.items()}
            w[parent] = w.get(parent, 0.0) + share
        out[v] = {b: x for b, x in w.items() if x > 0}
    return out


def rigid_weights(weights, parents, roots):
    """Every bone below one of `roots` gives its weight to that root, so the part follows the root as one piece (a
    cat's paw on the game creature's wrist, not on its clawed fingers). `weights`: per vertex {bone: weight};
    `parents`: bone -> parent (None at the root). Returns new per-vertex dicts."""
    def root_of(b):
        x = parents.get(b)
        while x is not None:
            if x in roots:
                return x
            x = parents.get(x)
        return None

    target = {b: root_of(b) for b in parents}
    out = []
    for ws in weights:
        new = {}
        for b, w in ws.items():
            t = target.get(b) or b
            new[t] = new.get(t, 0.0) + w
        out.append(new)
    return out


def clean_detail_weights(weights, in_tail, parents, main, keep, head):
    """Detail bones keep weight only where the creature has that detail. `weights`: per vertex {bone: weight};
    `in_tail`: per vertex, whether it is in the creature's tail; `parents`: bone -> parent (None at the root); `main`:
    the spine and leg bones; `keep`: the kept bones under the head (kept_head_bones); `head`: the head bone.
    A bone under the head that is not kept gives its weight to its nearest kept ancestor (or the head); any other
    bone off the main chains (a tail) keeps its weight on the tail and gives it elsewhere to its nearest main ancestor.
    Returns new per-vertex dicts."""
    def under_head(b):
        x = parents.get(b)
        while x is not None:
            if x == head:
                return True
            x = parents.get(x)
        return False

    def target(b, tail):
        if b in main or b in keep or b == head:
            return b
        if under_head(b):
            x = parents.get(b)
            while x is not None and x not in keep and x != head:
                x = parents.get(x)
            return x if x is not None else head
        if tail:
            return b
        x = parents.get(b)
        while x is not None and x not in main:
            x = parents.get(x)
        return x if x is not None else b

    out = []
    for ws, tail in zip(weights, in_tail):
        new = {}
        for b, w in ws.items():
            t = target(b, tail)
            new[t] = new.get(t, 0.0) + w
        out.append(new)
    return out
