"""Builds one reference plant (assets/plants/<plant>/) as a game asset.

  blender --background --factory-startup --python tools/blender/plant_builder.py -- <plant folder name>

Inputs: plant.json (species, growth form, size in metres, chosen sprites per role; tools/plants_specs.py) and the
packed atlas plant_albedo/normal/roughness.png + plant_uv.json (tools/plants_pack_sprites.py).
Geometry: modelled stems (tapered tubes, textured with the stem patch of the atlas) carrying cards cut from the
sprites: leaves folded along the midrib and curled, branch / sprig / frond cards as crossed pairs along the stems,
flowers / buds / fruits at the tips, grass blades and spikes for tufts. One material (alpha clip, double sided), so
the mod can draw it like the rebuilt grass. Units metres, Z up, origin at the foot (wall plants: on the wall, growing
toward -Y away from it). Writes <plant>.obj/.mtl, <plant>.blend and preview.png.
"""

import json
import math
import os
import random
import sys

import bpy
from mathutils import Matrix, Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ARGS = sys.argv[sys.argv.index("--") + 1:]
PLANT = ARGS[0]
VARIANT = int(ARGS[1]) if len(ARGS) > 1 else 1
DIR = os.path.join(ROOT, "assets", "plants", PLANT)
SPEC = json.load(open(os.path.join(DIR, "plant.json"), encoding="utf-8"))
UV = json.load(open(os.path.join(DIR, "plant_uv.json"), encoding="utf-8"))
import zlib  # noqa: E402

rng = random.Random(zlib.crc32(f"{PLANT}/{VARIANT}".encode()))
# variant 1: the spec's size; variant 2: a different individual - other seed, shorter and wider or taller and
# narrower (alternating by plant), other stem count
H, W = SPEC["height"], SPEC["width"]
STEMS = SPEC["stems"]
if VARIANT == 2:
    if zlib.crc32(PLANT.encode()) % 2:
        H, W = H * rng.uniform(0.75, 0.85), W * rng.uniform(1.1, 1.25)
    else:
        H, W = H * rng.uniform(1.12, 1.25), W * rng.uniform(0.8, 0.9)
    STEMS = max(3, int(round(STEMS * rng.uniform(0.7, 1.3))))
# Game version (third argument "game"): the same plant (same seed, same random draws) with fewer polygons for the
# in-game scatter - small cards with fewer segments (curl unreadable under ~6 cm), tiny ones flat, thin stems with
# fewer sides and rings. Written as <plant>_v<N>_game.obj and preview_v<N>_game.png.
GAME = len(ARGS) > 2 and ARGS[2] == "game"
NAME = f"{PLANT}_v{VARIANT}" + ("_game" if GAME else "")

# Real size (metres, card height) per sprite role: the sheets mix close-ups and whole sprigs, so the card size comes
# from what the sprite shows, not from the plant size (first build: Lycium berries and leaves 30 cm wide).
_ls = SPEC.get("leaf_size", [0.03, 0.06])
_fs = SPEC.get("flower_size", [0.03, 0.05])
SIZES = {"leaf": _ls, "leaf_dry": _ls, "leaf_compound": [_ls[0] * 2.5, _ls[1] * 2.5],
         "flower": _fs, "bud": [v * 0.6 for v in _fs], "fruit": SPEC.get("fruit_size", [0.02, 0.04]),
         "thorn": [0.015, 0.03], "twig": [0.12, 0.25], "sprig": [0.06, 0.12], "branch": [H * 0.2, H * 0.35],
         "frond": [H * 0.25, H * 0.4], "frond_thin": [H * 0.25, H * 0.4], "spike": [0.04, 0.07],
         "spike_dry": [0.04, 0.07], "blade": [H * 0.35, H * 0.75], "litter": [W * 0.4, W * 0.6], "stem": [0.1, 0.2]}
SIZES.update(SPEC.get("sizes", {}))
STEM_R = SPEC.get("stem_radius", 0.0035 + 0.006 * H)


def size_of(sprite, scale=1.0):
    lo, hi = SIZES.get(sprite["role"], [0.05, 0.1])
    return rng.uniform(lo, hi) * scale
UP = Vector((0, 0, 1))


def sprites(*roles):
    out = []
    for key, u in UV.items():
        if u["role"] in roles:
            out.append(u)
    return out


class MeshBuilder:
    def __init__(self):
        self.verts, self.faces, self.uvs = [], [], []

    def quad_grid(self, rows):
        """rows: list of rows of (position, uv); consecutive rows become quads."""
        base = len(self.verts)
        cols = len(rows[0])
        for row in rows:
            for p, uv in row:
                self.verts.append(tuple(p))
                self.uvs.append(uv)
        for r in range(len(rows) - 1):
            for c in range(cols - 1):
                a = base + r * cols + c
                self.faces.append((a, a + 1, a + cols + 1, a + cols))

    def card(self, base, up, facing, height, sprite, fold=0.0, curl=0.0, width_scale=1.0, segments=3):
        """A card standing on `base`, growing along `up`, its front toward `facing`. The sprite's bottom edge is at
        the base (stems drawn at the bottom of the sheet). fold: radians each half turns back along the midrib;
        curl: radians the tip bends back toward -facing."""
        up = up.normalized()
        side = up.cross(facing)
        if side.length < 1e-4:
            side = up.orthogonal()
        side.normalize()
        front = side.cross(up).normalized()
        width = height * sprite["aspect"] * width_scale
        u0, v0, u1, v1 = sprite["rect"]
        flat = False
        if GAME:
            segments = 1 if height < 0.07 else (2 if height < 0.2 else segments)
            flat = height < 0.045
        rows = []
        pos = base.copy()
        direction = up.copy()
        step = height / segments
        for s in range(segments + 1):
            t = s / segments
            v = 1.0 - (v1 - (v1 - v0) * t)   # image y grows downward; uv v grows upward
            half = side * (width / 2)
            lift = front * (-math.sin(fold) * width / 2)
            row = [(pos - half * math.cos(fold) + lift, (u0, v)),
                   (pos.copy(), ((u0 + u1) / 2, v)),
                   (pos + half * math.cos(fold) + lift, (u1, v))]
            if flat:
                row = [(pos - half, (u0, v)), (pos + half, (u1, v))]
            rows.append(row)
            if s < segments:
                rot = Matrix.Rotation(-curl / segments, 3, side)
                direction = (rot @ direction).normalized()
                pos = pos + direction * step
        self.quad_grid(rows)

    def crossed(self, base, up, height, sprite, yaw=None):
        """Two cards at 90 degrees around `up` (branch / sprig sprites: volume from every side)."""
        a = rng.uniform(0, math.pi) if yaw is None else yaw
        perp = up.orthogonal().normalized()
        f1 = Matrix.Rotation(a, 3, up.normalized()) @ perp
        f2 = Matrix.Rotation(a + math.pi / 2, 3, up.normalized()) @ perp
        self.card(base, up, f1, height, sprite, fold=0.08, curl=rng.uniform(0.0, 0.25))
        self.card(base, up, f2, height, sprite, fold=0.08, curl=rng.uniform(0.0, 0.25))

    def tube(self, points, r0, r1, sprite, sides=6):
        if GAME:
            sides = 3 if r0 < 0.0025 else (4 if r0 < 0.006 else sides)
            if len(points) > 5 and r0 < 0.006:
                points = points[::2] + ([points[-1]] if (len(points) - 1) % 2 else [])
        rows = []
        n = len(points)
        u0, v0, u1, v1 = sprite["rect"]
        length = 0.0
        for i in range(n):
            if i:
                length += (points[i] - points[i - 1]).length
            t = i / (n - 1)
            tangent = (points[min(i + 1, n - 1)] - points[max(i - 1, 0)]).normalized()
            a = tangent.orthogonal().normalized()
            b = tangent.cross(a).normalized()
            r = r0 + (r1 - r0) * t
            v = 1.0 - (v0 + (v1 - v0) * ((length * 2.0) % 1.0))  # bark repeats every 0.5 m inside the patch
            row = []
            for k in range(sides + 1):
                ang = 2 * math.pi * k / sides
                row.append((points[i] + (a * math.cos(ang) + b * math.sin(ang)) * r, (u0 + (u1 - u0) * k / sides, v)))
            rows.append(row)
        self.quad_grid(rows)


def grow_stem(start, direction, length, droop=0.0, wiggle=0.15, segments=10):
    """Polyline from start along direction, bending down (droop) and wandering."""
    pts = [start.copy()]
    d = direction.normalized()
    step = length / segments
    for _ in range(segments):
        d = (d + Vector((rng.gauss(0, wiggle), rng.gauss(0, wiggle), rng.gauss(0, wiggle * 0.5))) * 0.3
             - UP * droop / segments).normalized()
        pts.append(pts[-1] + d * step)
    return pts


def along(pts, t):
    """Position and tangent at parameter t in [0, 1] along a polyline."""
    f = t * (len(pts) - 1)
    i = min(int(f), len(pts) - 2)
    k = f - i
    p = pts[i].lerp(pts[i + 1], k)
    return p, (pts[i + 1] - pts[i]).normalized()


def leaf_dir(tangent, k, outward=0.85):
    """Leaf axis leaving the stem at an angle, phyllotaxis 137.5 degrees."""
    perp = tangent.orthogonal().normalized()
    radial = Matrix.Rotation(math.radians(137.5) * k, 3, tangent) @ perp
    return (tangent * (1 - outward) + radial * outward).normalized(), radial


def dress_stem(mb, pts, k0, leaf_density, leaf_roles=("leaf",), t_range=(0.08, 0.95), size=None,
               card_roles=(), card_every=0.0, card_size=(0.15, 0.3), tip_roles=(), tip_size=None, tip_count=1,
               taper=0.0, tip_t=(0.4, 0.98)):
    leaves = sprites(*leaf_roles)
    cards = sprites(*card_roles) if card_roles else []
    tips = sprites(*tip_roles) if tip_roles else []
    length = sum((pts[i + 1] - pts[i]).length for i in range(len(pts) - 1))
    n = int(length * leaf_density)
    for k in range(n if leaves else 0):
        t = rng.uniform(*t_range)
        p, tan = along(pts, t)
        d, radial = leaf_dir(tan, k0 + k)
        facing = (UP * 0.5 + radial * 0.5).normalized()
        lf = rng.choice(leaves)
        mb.card(p, d, facing, size_of(lf, (1.3 - 0.6 * t) * (1.0 - taper * t)), lf,
                fold=rng.uniform(0.12, 0.35), curl=rng.uniform(0.1, 0.5))
    if cards and card_every > 0:
        m = max(1, int(length / card_every))
        for k in range(m):
            t = rng.uniform(0.15, 0.95)
            p, tan = along(pts, t)
            d, _ = leaf_dir(tan, k, outward=0.45 + 0.35 * taper * (1.0 - t))
            c = rng.choice(cards)
            mb.crossed(p, d, size_of(c, 1.0 - taper * t), c)
    fsize = tip_size or SPEC.get("flower_size", [0.04, 0.06])
    for k in range(tip_count if tips else 0):
        t = 1.0 if k == 0 else rng.uniform(*tip_t)
        p, tan = along(pts, t)
        _, radial = leaf_dir(tan, k * 3 + 1)
        facing = (radial * 0.65 + UP * 0.35).normalized()
        up = (UP - facing * facing.dot(UP)).normalized()
        if up.length < 1e-3:
            up = tan.orthogonal()
        tp = rng.choice(tips)
        s_ = size_of(tp)
        if tp["role"] == "flower" and SPEC.get("bloom_3d"):
            c = p + (UP * 0.6 + radial * 0.4).normalized() * 0.01
            yaw = rng.uniform(0, math.pi)
            flat_up = Matrix.Rotation(yaw, 3, UP) @ Vector((1, 0, 0))
            mb.card(c - flat_up * (s_ * 0.5) + UP * 0.004, flat_up, UP, s_, tp, fold=-0.25, curl=0.0)
            for a2 in (0.0, math.pi / 2):
                f = (Matrix.Rotation(yaw + a2, 3, UP) @ Vector((1, 0, 0)) + UP * 1.2).normalized()
                u2 = (UP - f * f.dot(UP)).normalized()
                mb.card(c - u2 * (s_ * 0.45), u2, f, s_ * 0.9, tp, fold=-0.2, curl=0.0)
            continue
        mb.card(p + radial * 0.015 - up * (s_ * 0.5), up, facing, s_, tp, fold=0.05, curl=0.05)


def stem_sprite():
    return next(u for u in UV.values() if u["role"] == "stem")


def build():
    mb = MeshBuilder()
    form = SPEC["form"]
    n = STEMS
    stem_tex = stem_sprite()
    flower_roles = [r for r in ("flower", "bud", "fruit") if sprites(r)]
    leaf_roles = [r for r in ("leaf", "leaf_dry", "leaf_compound") if sprites(r)]
    card_roles = [r for r in ("branch", "sprig", "frond", "frond_thin") if sprites(r)]
    k0 = 0
    if form == "leader":
        # young tree: one straight leader, side shoots at regular nodes (phyllotaxis), ascending 35-55 degrees,
        # shorter toward the top; leafy branch cards on the shoots, twin basal suckers
        lead = grow_stem(Vector((0, 0, 0)), Vector((rng.gauss(0, 0.04), rng.gauss(0, 0.04), 1)), H, droop=0.0,
                         wiggle=0.03, segments=12)
        mb.tube(lead, STEM_R, STEM_R * 0.3, stem_tex)
        nodes = SPEC.get("nodes", 14)
        for j in range(nodes):
            t = 0.12 + 0.86 * j / nodes + rng.uniform(-0.02, 0.02)
            p, tan = along(lead, t)
            radial = Matrix.Rotation(math.radians(137.5) * j, 3, UP) @ Vector((1, 0, 0))
            ang = math.radians(rng.uniform(35, 55))
            d = (radial * math.sin(ang) + UP * math.cos(ang)).normalized()
            length = W * 0.55 * (1.05 - t) + 0.08
            sub = grow_stem(p, d, length, droop=0.05, wiggle=0.06, segments=5)
            mb.tube(sub, STEM_R * 0.45, STEM_R * 0.15, stem_tex)
            dress_stem(mb, sub, k0, SPEC.get("leaf_density", 50), leaf_roles, t_range=(0.2, 1.0),
                       card_roles=card_roles, card_every=SPEC.get("card_every", 0.14))
            k0 += 7
        dress_stem(mb, lead, k0, SPEC.get("leaf_density", 50) * 0.6, leaf_roles, t_range=(0.5, 1.0),
                   card_roles=card_roles, card_every=0.25)
    elif form == "rose":
        # rose bush: arching canes from the crown, laterals ending in blooms with buds beside them
        for i in range(n):
            a = 2 * math.pi * i / n + rng.uniform(-0.4, 0.4)
            ang = math.radians(rng.uniform(10, 40))
            d = Vector((math.cos(a) * math.sin(ang), math.sin(a) * math.sin(ang), math.cos(ang)))
            cane = grow_stem(Vector((math.cos(a), math.sin(a), 0)) * 0.05, d, H * rng.uniform(0.8, 1.15),
                             droop=0.9, wiggle=0.08, segments=12)
            mb.tube(cane, STEM_R, STEM_R * 0.4, stem_tex)
            dress_stem(mb, cane, k0, SPEC.get("leaf_density", 30), leaf_roles, t_range=(0.15, 0.95))
            for j in range(rng.randint(2, 4)):
                t = rng.uniform(0.45, 0.9)
                p, tan = along(cane, t)
                dd, _ = leaf_dir(tan, j * 2 + i, outward=0.6)
                lat = grow_stem(p, (dd + UP * 0.8).normalized(), rng.uniform(0.12, 0.25), droop=0.1, segments=4)
                mb.tube(lat, STEM_R * 0.5, STEM_R * 0.3, stem_tex)
                dress_stem(mb, lat, k0 + j, SPEC.get("leaf_density", 30) * 1.5, leaf_roles, t_range=(0.1, 0.8),
                           tip_roles=["flower"], tip_count=1)
                if sprites("bud"):
                    dress_stem(mb, lat, k0 + j + 5, 0, leaf_roles, tip_roles=["bud"], tip_count=rng.randint(1, 3),
                               tip_t=(0.75, 0.95))
            k0 += 13
    elif form in ("upright", "shrub", "herb"):
        for i in range(n):
            a = 2 * math.pi * i / n + rng.uniform(-0.4, 0.4)
            spread = math.radians(rng.uniform(*SPEC.get("spread", [8, 32] if form == "upright" else [10, 45])))
            d = Vector((math.cos(a) * math.sin(spread), math.sin(a) * math.sin(spread), math.cos(spread)))
            base = Vector((math.cos(a), math.sin(a), 0)) * rng.uniform(0, W * 0.08)
            L = H * rng.uniform(0.65, 1.0) / max(math.cos(spread), 0.5)
            pts = grow_stem(base, d, L, droop=SPEC.get("droop", 0.15 if form == "upright" else 0.25),
                            wiggle=SPEC.get("wiggle", 0.15))
            mb.tube(pts, STEM_R, STEM_R * 0.3, stem_tex)
            if form == "shrub":  # side branches carrying most of the foliage
                for j in range(rng.randint(*SPEC.get("side_branches", [3, 6]))):
                    t = rng.uniform(0.25, 0.85)
                    p, tan = along(pts, t)
                    dd, _ = leaf_dir(tan, j, outward=0.7)
                    sub = grow_stem(p, dd, L * rng.uniform(0.25, 0.45), droop=0.2, segments=6)
                    mb.tube(sub, STEM_R * 0.55, STEM_R * 0.2, stem_tex)
                    dress_stem(mb, sub, k0, SPEC.get("leaf_density", 70), leaf_roles, card_roles=card_roles,
                               card_every=SPEC.get("card_every", 0.18), card_size=(H * 0.12, H * 0.22),
                               tip_roles=flower_roles, tip_count=2)
                    k0 += 11
            fps = SPEC.get("flowers_per_stem", 4)
            dress_stem(mb, pts, k0, SPEC.get("leaf_density", 32 if form == "upright" else 26), leaf_roles,
                       card_roles=card_roles, card_every=SPEC.get("card_every", H * 0.25), card_size=(H * 0.18, H * 0.32), tip_roles=flower_roles,
                       tip_count=rng.randint(max(1, fps - 2), fps + 2), taper=SPEC.get("taper", 0.0),
                       tip_t=tuple(SPEC.get("tip_t", [0.4, 0.98])))
            k0 += 17
    elif form in ("dome", "cushion"):
        for i in range(n):
            a = rng.uniform(0, 2 * math.pi)
            el = math.radians(rng.uniform(15, 85))
            d = Vector((math.cos(a) * math.cos(el), math.sin(a) * math.cos(el), math.sin(el)))
            # reach the ellipsoid (W/2, W/2, H)
            reach = 1.0 / math.sqrt((d.x / (W / 2)) ** 2 + (d.y / (W / 2)) ** 2 + (d.z / H) ** 2)
            pts = grow_stem(Vector((0, 0, 0)), (d + UP * 0.6).normalized(), reach * rng.uniform(0.85, 1.05),
                            droop=0.1, wiggle=0.25)
            mb.tube(pts, STEM_R, STEM_R * 0.3, stem_tex)
            dress_stem(mb, pts, k0, 140 if form == "dome" else 90, leaf_roles, t_range=(0.4, 1.0),
                       card_roles=card_roles + (["twig", "thorn"] if form == "cushion" else []),
                       card_every=0.07, card_size=(H * 0.18, H * 0.4), tip_roles=flower_roles,
                       tip_count=rng.randint(1, SPEC.get("flowers_per_stem", 3)))
            k0 += 13
        # shell: sprig / branch cards over the ellipsoid surface, rooted a little inside and pointing out and up,
        # so the dome reads as a closed, dense crown like the photos (stems only seen through gaps)
        shell = sprites(*card_roles) or sprites(*leaf_roles)
        count = int(SPEC.get("shell_density", 480 if form == "dome" else 260) * (W * W + 2 * W * H) / 1.2)
        tips = sprites(*flower_roles) if flower_roles else []
        for k in range(count):
            a = rng.uniform(0, 2 * math.pi)
            el = math.asin(rng.uniform(0.05, 1.0))          # uniform over the upper half
            nrm = Vector((math.cos(a) * math.cos(el) / (W / 2), math.sin(a) * math.cos(el) / (W / 2),
                          math.sin(el) / H)).normalized()
            p = Vector((math.cos(a) * math.cos(el) * W / 2, math.sin(a) * math.cos(el) * W / 2, math.sin(el) * H))
            inner = k % 3 == 0          # a third of the cards fill the core, so the crown does not look hollow
            p = p * (rng.uniform(0.35, 0.6) if inner else rng.uniform(0.62, 0.82))
            up = (nrm + UP * (0.9 - 0.4 * math.sin(el)) + Vector((rng.gauss(0, 0.2), rng.gauss(0, 0.2), 0))).normalized()
            c = rng.choice(shell)
            mb.crossed(p, up, size_of(c), c)
            if tips and rng.random() < SPEC.get("shell_flowers", 0.12):
                tp = rng.choice(tips)
                s_ = size_of(tp)
                q = p + up * rng.uniform(0.1, 0.3) * H
                f = (nrm * 0.6 + UP * 0.4).normalized()
                mb.card(q, (UP - f * f.dot(UP)).normalized(), f, s_, tp, fold=0.05, curl=0.05)
    elif form == "trailing":
        for i in range(n):
            a = 2 * math.pi * i / n + rng.uniform(-0.3, 0.3)
            d = Vector((math.cos(a), math.sin(a), rng.uniform(0.3, 0.7)))
            pts = grow_stem(Vector((0, 0, H * 0.3)), d, W * rng.uniform(0.35, 0.6), droop=0.9, wiggle=0.2)
            mb.tube(pts, 0.006, 0.002, stem_tex)
            dress_stem(mb, pts, k0, SPEC.get("leaf_density", 70), leaf_roles, card_roles=card_roles,
                       card_every=SPEC.get("card_every", 0.2),
                       card_size=(H * 0.4, H * 0.7), tip_roles=flower_roles,
                       tip_count=rng.randint(2, SPEC.get("flowers_per_stem", 4)))
            k0 += 9
        twigs = sprites("twig")
        side = rng.uniform(0, 2 * math.pi)
        for k in range(SPEC.get("twig_count", 14) if twigs else 0):  # dry wood tangle on one side, like the photo
            a = side + rng.uniform(-0.8, 0.8)
            base = Vector((math.cos(a), math.sin(a), 0)) * rng.uniform(W * 0.1, W * 0.3)
            tw = rng.choice(twigs)
            mb.crossed(base, Vector((math.cos(a), math.sin(a), rng.uniform(0.2, 0.9))), size_of(tw), tw)
    elif form in ("wall", "wall_rosette"):
        out = Vector((0, -1, 0))   # away from the wall (wall plane y = 0)
        for i in range(SPEC.get("rosette_count", n) if form == "wall_rosette" else n):
            a = rng.gauss(0, 0.75)
            el = rng.gauss(0.55, 0.55)
            d = (out + Vector((math.sin(a), 0, 0)) + UP * el).normalized()
            if form == "wall_rosette":
                fr = sprites("frond", "leaf")
                length = H * (rng.uniform(0.25, 0.55) if rng.random() < 0.45 else rng.uniform(0.6, 1.15))
                base = Vector((rng.gauss(0, 0.02), -0.01, rng.gauss(0, 0.02)))
                mb.card(base, d, (UP * rng.uniform(0.2, 0.9) + out).normalized(), length, rng.choice(fr),
                        fold=rng.uniform(0.05, 0.35), curl=rng.uniform(-0.2, 1.2), width_scale=rng.uniform(0.4, 0.75))
                continue
            pts = grow_stem(Vector((0, -0.01, 0)), d, H * rng.uniform(0.5, 1.0), droop=0.15, wiggle=0.2, segments=8)
            mb.tube(pts, 0.004, 0.0015, stem_tex)
            dress_stem(mb, pts, k0, 90, leaf_roles, card_roles=card_roles, card_every=0.12,
                       card_size=(H * 0.2, H * 0.35))
            k0 += 7
    elif form == "tuft":
        blades, spikes = sprites("blade"), sprites("spike", "spike_dry")
        stems_cards = sprites("branch")
        for i in range(n):
            a = rng.uniform(0, 2 * math.pi)
            r = abs(rng.gauss(0, W * 0.08))
            base = Vector((math.cos(a) * r, math.sin(a) * r, -0.01))
            lean = math.radians(rng.uniform(0, 35))
            d = Vector((math.cos(a) * math.sin(lean), math.sin(a) * math.sin(lean), math.cos(lean)))
            f = Matrix.Rotation(rng.uniform(0, math.pi), 3, UP) @ Vector((1, 0, 0))
            if i % 3 == 0 and stems_cards:
                mb.card(base, d, f, H * rng.uniform(0.75, 1.0), rng.choice(stems_cards), fold=0.0, curl=0.15)
            else:
                mb.card(base, d, f, H * rng.uniform(0.35, 0.75), rng.choice(blades), fold=0.1,
                        curl=rng.uniform(0.3, 1.1), width_scale=1.6)
        for i in range(n // 3):  # modelled culms with an ear
            a = rng.uniform(0, 2 * math.pi)
            lean = math.radians(rng.uniform(5, 30))
            d = Vector((math.cos(a) * math.sin(lean), math.sin(a) * math.sin(lean), math.cos(lean)))
            pts = grow_stem(Vector((0, 0, 0)), d, H * rng.uniform(0.6, 0.9), droop=0.1, wiggle=0.08, segments=6)
            mb.tube(pts, 0.0018, 0.001, stem_tex, sides=4)
            p, tan = along(pts, 1.0)
            mb.crossed(p - tan * 0.01, tan, H * rng.uniform(0.18, 0.28), rng.choice(spikes))
        for lt in sprites("litter"):
            mb.card(Vector((0, 0, 0.005)), Vector((1, 0, 0.02)), UP, size_of(lt), lt, fold=0.0)
    return mb


def material():
    mat = bpy.data.materials.new(PLANT)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    def tex(name, data):
        t = nt.nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(os.path.join(DIR, name), check_existing=True)
        if data:
            t.image.colorspace_settings.name = "Non-Color"
        return t
    a = tex("plant_albedo.png", False)
    nt.links.new(a.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(a.outputs["Alpha"], bsdf.inputs["Alpha"])
    nt.links.new(tex("plant_roughness.png", True).outputs["Color"], bsdf.inputs["Roughness"])
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(tex("plant_normal.png", True).outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def preview():
    scene = bpy.context.scene
    world = bpy.data.worlds.new("sky")
    scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.55, 0.6, 0.7, 1)
    bg.inputs["Strength"].default_value = 0.6
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    bpy.ops.mesh.primitive_plane_add(size=20)
    g = bpy.data.materials.new("ground")
    g.use_nodes = True
    next(n for n in g.node_tree.nodes if n.type == "BSDF_PRINCIPLED").inputs["Base Color"].default_value = (0.45, 0.38, 0.28, 1)
    bpy.context.active_object.data.materials.append(g)
    if SPEC["form"].startswith("wall"):
        bpy.ops.mesh.primitive_plane_add(size=6, location=(0, 0.005, 1.5), rotation=(math.radians(90), 0, 0))
        bpy.context.active_object.data.materials.append(g)
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 48
    scene.cycles.transparent_max_bounces = 64
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception:
        pass
    scene.render.resolution_x = scene.render.resolution_y = 1024
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    size = max(H, W)
    target = Vector((0, 0, H * 0.5))
    cam.location = target + Vector((0.35, -1.0, 0.3)).normalized() * size * 2.0
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = 50
    scene.render.filepath = os.path.join(DIR, f"preview_v{VARIANT}" + ("_game" if GAME else "") + ".png")
    bpy.ops.render.render(write_still=True)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mb = build()
    mesh = bpy.data.meshes.new(NAME)
    mesh.from_pydata(mb.verts, [], mb.faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for poly in mesh.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = mb.uvs[mesh.loops[li].vertex_index]
    mesh.update()
    obj = bpy.data.objects.new(NAME, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.data.materials.append(material())
    print(f"{NAME}: {len(mesh.polygons) * 2} triangles, {H:.2f} x {W:.2f} m, {STEMS} stems")
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.wm.obj_export(filepath=os.path.join(DIR, NAME + ".obj"), export_selected_objects=True,
                          forward_axis="Y", up_axis="Z", export_materials=True, path_mode="RELATIVE")
    preview()
    if GAME:
        return
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(DIR, NAME + ".blend"))


main()
