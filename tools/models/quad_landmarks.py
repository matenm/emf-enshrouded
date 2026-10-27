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
    behind as low as the feet (Meshy's panther's) does not confuse them.
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
