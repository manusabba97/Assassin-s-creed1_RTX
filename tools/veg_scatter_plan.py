"""Natural vegetation for the level from the rebuilt plants (user 2026-10-03: "use the places of the original grass and
put the plants we made there, not 1-1 but in the most natural way, by number and size").

  python tools/veg_scatter_plan.py [--pack] [--preview]

Reads the engine's grass tufts (clutter mesh 2A7F3012BFA4FEBD) from pbr/vegetation/placements.txt (ExportVegetation)
and writes pbr/vegetation/scatter/<id>/ for the mod (remix.cpp, vegetation scatter):
  plants.txt  one line per mesh: "<folder> <obj>"; each folder holds the plant's maps (albedo / normal / roughness
              .ac1t, 2048, packed with --pack) and its game meshes (plant_builder.py ... game).
  plan.txt    "T x y z" the tufts (the mod recognises the level by them), "P mesh range m00..m23" every planned plant
              (world 3x4), "F template mesh dx dy yaw scale" small templates for tufts outside the plan.
The ground the engine covered with grass (discs around the tufts) is the habitat. Plants are laid out like a
Mediterranean slope: species in clumps (parent points + children around them, Neyman-Scott), each species where its
habitat field is high (smooth noise: meadow, dry ground, scrub), big plants first, no plant inside another's crown.
Ground height and slope from the nearest tufts (their matrices carry the terrain's normal). Previews:
assets/plants/_review/scatter_map.png (top view) and scatter_area.png (a Blender render of one area, --preview).
"""

import argparse
import math
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pbr_pack import FMT_BC3, FMT_BC5, PBR, bleed_colour, encode, mip_chain, octahedral, write  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PLANTS = ROOT / "assets" / "plants"
CLUTTER = "2A7F3012BFA4FEBD"
OUT = PBR / "vegetation" / "scatter" / CLUTTER
GRASS = PBR / "vegetation" / "replace" / CLUTTER   # the rebuilt grass clump (grass_modern.py), approved 2026-10-03
TEX = 2048
REACH = 0.65      # habitat: within this distance of a tuft (the original clump is 1.2 m wide)
rng = np.random.default_rng(20261003)

# species: folder, density per m2 of habitat, crown radius m (spacing), clump (children per parent, sigma m),
# habitat weights (meadow, dry, scrub, base), draw range m, scale range, sink m.
# Densities: a grazed Mediterranean slope - continuous grass, barley grass in patches, scattered weeds and flowers,
# low spiny cushions and thyme in scrub patches, a few big shrubs. Wall plants (caper, Parietaria, the rosette) are
# not used here: the grass tufts are on open ground.
SPECIES = [
    # big shrubs and tall herbs first
    dict(name="13_Lycium_europaeum", dens=0.012, r=0.8, kids=(1, 2), sigma=1.6, hab=(0.0, 0.3, 1.0, 0.1), range=150, scale=(0.8, 1.1), sink=0.06),
    dict(name="14_ALBERELLO", dens=0.005, r=0.5, kids=(1, 2), sigma=2.0, hab=(0.2, 0.0, 0.8, 0.1), range=150, scale=(0.75, 1.1), sink=0.05),
    dict(name="15_Rosa_damascena", dens=0.004, r=0.6, kids=(1, 1), sigma=1.0, hab=(0.6, 0.0, 0.4, 0.05), range=150, scale=(0.8, 1.05), sink=0.05),
    dict(name="7_Foeniculum_vulgare", dens=0.024, r=0.45, kids=(3, 7), sigma=0.8, hab=(0.5, 0.5, 0.0, 0.1), range=120, scale=(0.75, 1.1), sink=0.03),
    dict(name="5_FIORI", dens=0.06, r=0.3, kids=(6, 14), sigma=0.6, hab=(0.3, 0.6, 0.2, 0.1), range=70, scale=(0.8, 1.15), sink=0.03),
    dict(name="12_Thymbra_capitata", dens=0.06, r=0.35, kids=(5, 12), sigma=0.9, hab=(0.0, 0.7, 0.8, 0.0), range=70, scale=(0.75, 1.15), sink=0.04),
    dict(name="10_Sarcopoterium_spinosum", dens=0.05, r=0.38, kids=(5, 12), sigma=0.9, hab=(0.0, 0.6, 1.0, 0.0), range=70, scale=(0.75, 1.15), sink=0.04),
    dict(name="11_Sarcopoterium_spinosum", dens=0.05, r=0.38, kids=(5, 12), sigma=0.9, hab=(0.0, 0.6, 1.0, 0.0), range=70, scale=(0.75, 1.15), sink=0.04),
    # herbs and flowers
    dict(name="4_FIORI", dens=0.1, r=0.28, kids=(8, 18), sigma=0.5, hab=(1.0, 0.0, 0.0, 0.1), range=45, scale=(0.75, 1.1), sink=0.02),
    dict(name="8_Chenopodium_murale", dens=0.1, r=0.25, kids=(5, 12), sigma=0.5, hab=(0.6, 0.3, 0.0, 0.2), range=35, scale=(0.8, 1.15), sink=0.02),
    dict(name="6_Peganum_harmala", dens=0.1, r=0.2, kids=(6, 14), sigma=0.55, hab=(0.0, 1.0, 0.2, 0.1), range=35, scale=(0.85, 1.2), sink=0.02),
    # ground cover last
    dict(name="9_Hordeum_murinum", dens=1.4, r=0.18, kids=(15, 35), sigma=0.55, hab=(0.4, 0.8, 0.1, 0.2), range=30, scale=(0.8, 1.2), sink=0.02),
    dict(name="grass", dens=2, r=0.42, kids=(6, 14), sigma=0.7, hab=(1.0, 0.3, 0.2, 0.35), range=35, scale=(0.7, 1.15), sink=0.02),
]
# uniform scale per plant (assets/plants/scatter_scale.json, sizes drawn by the user on the lineup): crown radius,
# clump spread and sink grow with it, density falls with it
import json  # noqa: E402
BASE = json.load(open(PLANTS / "scatter_scale.json"))
for _sp in SPECIES:
    _b = BASE.get(_sp["name"], 1.0)
    _sp["scale"] = (_sp["scale"][0] * _b, _sp["scale"][1] * _b)
    _sp["r"] *= _b
    _sp["sigma"] *= _b ** 0.5
    _sp["sink"] *= _b
    _sp["dens"] /= _b  # (not the square: denser patches, user 2026-10-03)
    _sp["range"] = max(_sp["range"], min(200, _sp["range"] * _b)) if _b > 1 else _sp["range"]
V2_SCALE = 1.0  # variant 2 scale (1.15 asked and undone, 2026-10-03)
BIG = {"13_Lycium_europaeum", "14_ALBERELLO", "15_Rosa_damascena"}
# tufts outside the plan: low ground cover only (a shrub per tuft by position would land on paths and repeat)
TEMPLATE_MIX = [("grass", 0.5), ("9_Hordeum_murinum", 0.3), ("8_Chenopodium_murale", 0.08), ("6_Peganum_harmala", 0.07),
                ("4_FIORI", 0.05)]


def tufts():
    rows = [l.split() for l in open(PBR / "vegetation" / "placements.txt") if l.startswith(CLUTTER)]
    m = np.array([[float(x) for x in r[1:]] for r in rows], np.float64).reshape(-1, 3, 4)
    _, keep = np.unique(np.round(m[:, :, 3] / 0.1).astype(np.int64), axis=0, return_index=True)
    return m[np.sort(keep)]


class Noise:
    """Smooth value noise (two octaves) over the level, one per habitat field."""

    def __init__(self, scale):
        self.scale = scale
        self.grid = rng.random((512, 512))
        self.off = rng.random(2) * 200

    def _octave(self, x, y, s):
        gx, gy = x / s + self.off[0], y / s + self.off[1]
        ix, iy = np.floor(gx).astype(int), np.floor(gy).astype(int)
        fx, fy = gx - ix, gy - iy
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        g = lambda a, b: self.grid[a % 512, b % 512]  # noqa: E731
        return (g(ix, iy) * (1 - fx) * (1 - fy) + g(ix + 1, iy) * fx * (1 - fy) + g(ix, iy + 1) * (1 - fx) * fy +
                g(ix + 1, iy + 1) * fx * fy)

    def __call__(self, x, y):
        v = 0.7 * self._octave(x, y, self.scale) + 0.3 * self._octave(x, y, self.scale * 0.35)
        return np.clip((v - 0.5) * 2.2 + 0.5, 0, 1)


MEADOW, DRY, SCRUB = Noise(14.0), Noise(22.0), Noise(18.0)


def habitat(sp, x, y):
    wm, wd, ws, base = sp["hab"]
    v = base + wm * MEADOW(x, y) + wd * DRY(x, y) + ws * SCRUB(x, y) ** 1.5
    return v / (base + wm + wd + ws)


def ground(m, tree, x, y):
    """Height and normal of the ground at (x, y): the planes of the 3 nearest tufts, inverse-distance weighted."""
    d, i = tree.query(np.stack([x, y], -1), k=3)
    w = 1.0 / np.maximum(d, 0.05)
    w /= w.sum(-1, keepdims=True)
    t = m[i]                                       # (n, 3, 3, 4)
    n = t[..., :, 2]                               # tuft up axis = terrain normal
    n = n / np.linalg.norm(n, axis=-1, keepdims=True)
    nz = np.maximum(n[..., 2], 0.35)
    z = t[..., 2, 3] - (n[..., 0] * (x[:, None] - t[..., 0, 3]) + n[..., 1] * (y[:, None] - t[..., 1, 3])) / nz
    nrm = (n * w[..., None]).sum(1)
    return (z * w).sum(-1), nrm / np.linalg.norm(nrm, axis=-1, keepdims=True), z.max(-1) - z.min(-1)


def frame(up, yaw, s):
    """3x3 (columns forward, side, up), up = world up bent a quarter toward the terrain normal, like the mod."""
    u = np.array([0, 0, 1.0]) * 0.75 + up * 0.25
    u /= np.linalg.norm(u)
    ref = np.array([math.cos(yaw), math.sin(yaw), 0.0])
    side = np.cross(u, ref)
    side /= np.linalg.norm(side)
    fwd = np.cross(side, u)
    return np.stack([fwd, side, u], 1) * s


def candidates(sp, hab_cells, w, target, kmean):
    """Clumps (parent + children around it), in batches until the caller has its target."""
    for _ in range(8):
        pts = []
        for p in hab_cells[rng.choice(len(hab_cells), max(1, int(round(target / kmean))), p=w)]:
            k = rng.integers(sp["kids"][0], sp["kids"][1] + 1)
            pts.append(p + rng.normal(0, sp["sigma"], (k, 2)) * np.array([[0.0, 0.0]] + [[1, 1]] * (k - 1)))
        pts = np.concatenate(pts)
        pts += rng.uniform(-0.12, 0.12, pts.shape)
        rng.shuffle(pts)
        yield from pts


# The game's bushes (static alpha-tested meshes with the leafy shrub textures, sorted on
# assets/plants/_review/static_veg_sheet.jpg: 5967DBC0 leafy branches, 268C0698 flowering sprigs; 1-2.4 m high, on the
# ground). The mod hides them (scatter/removed.txt) and the plan puts a group of our plants where each stood: one
# shrub in the middle, smaller plants around it within the old crown.
BUSH_ALBEDO = {"5967DBC0BF198086", "268C0698970B65CA"}
BUSH_CENTRE = [("13_Lycium_europaeum", 3), ("12_Thymbra_capitata", 3), ("10_Sarcopoterium_spinosum", 2),
               ("11_Sarcopoterium_spinosum", 2), ("15_Rosa_damascena", 1), ("14_ALBERELLO", 1), ("5_FIORI", 2),
               ("7_Foeniculum_vulgare", 1), ("4_FIORI", 1)]
BUSH_SMALL = [("8_Chenopodium_murale", 2), ("6_Peganum_harmala", 2), ("9_Hordeum_murinum", 3), ("grass", 3),
              ("4_FIORI", 1), ("12_Thymbra_capitata", 1), ("10_Sarcopoterium_spinosum", 1)]


def bushes():
    """(geometry ids, sites): sites = (x, y, z, crown radius, up axis) per distinct placement."""
    veg = PBR / "vegetation"
    ids = {}
    for line in open(veg / "index.txt"):
        f = line.split()
        if f[1] == "static" and f[7] in BUSH_ALBEDO and f[0] not in ids:
            v = np.array([[float(x) for x in l.split()[1:4]] for l in open(veg / f"{f[0]}.obj") if l.startswith("v ")])
            ext = v.max(0) - v.min(0)
            ids[f[0]] = max(ext[0], ext[1]) / 2
    rows = [l.split() for l in open(veg / "placements.txt") if l.split()[0] in ids]
    sites = []
    for r in rows:
        mat = np.array([float(x) for x in r[1:]]).reshape(3, 4)
        scale = np.linalg.norm(mat[:, :3], axis=0).mean()
        up = mat[:, 2] / np.linalg.norm(mat[:, 2])
        sites.append((mat[0, 3], mat[1, 3], mat[2, 3], ids[r[0]] * scale, up))
    pos = np.array([q[:2] for q in sites])
    _, keep = np.unique(np.round(pos / 0.3).astype(np.int64), axis=0, return_index=True)
    return sorted(ids), [sites[i] for i in sorted(keep)]


def pick(table):
    names = [t[0] for t in table]
    w = np.array([t[1] for t in table], float)
    return names[rng.choice(len(names), p=w / w.sum())]


def bush_plants(sites):
    """Our plants where the bushes were: (name, x, y, z, up) - the centre shrub, then 3-8 smaller ones in the crown."""
    out = []
    for x, y, z, r, up in sites:
        out.append((pick(BUSH_CENTRE), x, y, z, up))
        for _ in range(rng.integers(3, 9)):
            a, d = rng.uniform(0, 2 * math.pi), r * math.sqrt(rng.uniform(0.15, 1.0))
            out.append((pick(BUSH_SMALL), x + d * math.cos(a), y + d * math.sin(a), z, up))
    return out


def plan(m):
    pos = m[:, :2, 3]
    tree = cKDTree(pos)
    # habitat area: 0.25 m cells within REACH of a tuft
    lo, hi = pos.min(0) - 1, pos.max(0) + 1
    gx, gy = np.meshgrid(np.arange(lo[0], hi[0], 0.25), np.arange(lo[1], hi[1], 0.25))
    cells = np.stack([gx.ravel(), gy.ravel()], -1)
    inside = tree.query(cells, distance_upper_bound=REACH)[0] < REACH
    area = inside.sum() * 0.0625
    hab_cells = cells[inside]
    print(f"{len(m)} tufts, habitat {area:.0f} m2")
    placed = []           # (x, y, r, big)
    placed_tree = None
    out = []
    spec = {sp["name"]: sp for sp in SPECIES}
    _, sites = bushes()
    for name, x, y, z, up in bush_plants(sites):
        sp = spec[name]
        v = int(rng.integers(0, 2))
        s = rng.uniform(*sp["scale"]) * (V2_SCALE if v else 1.0)
        placed.append((x, y, sp["r"], name in BIG))
        out.append((name, v, frame(up, rng.uniform(0, 2 * math.pi), s), np.array([x, y, z - sp["sink"]]),
                    sp["range"]))
    print(f"{len(sites)} bushes replaced by {len(out)} plants")
    for sp in SPECIES:
        target = int(round(sp["dens"] * area))
        w = habitat(sp, hab_cells[:, 0], hab_cells[:, 1]) ** 4
        w /= w.sum()
        kmean = sum(sp["kids"]) / 2
        coords = np.array([[q[0], q[1]] for q in placed]) if placed else np.zeros((0, 2))
        radii = np.array([q[2] for q in placed]) if placed else np.zeros(0)
        placed_tree = cKDTree(coords) if len(coords) else None
        new = []
        for x, y in candidates(sp, hab_cells, w, target, kmean):
            if len(new) >= target:
                break
            if tree.query([x, y])[0] > REACH:
                continue
            if rng.random() > 0.35 + 0.65 * habitat(sp, np.array([x]), np.array([y]))[0]:
                continue
            ok = True
            if placed_tree is not None:
                for j in placed_tree.query_ball_point([x, y], sp["r"] + 0.85):
                    big = placed[j][3]
                    # small plants may grow under a big shrub's edge, not in its centre
                    lim = (radii[j] * 0.55 if (big and sp["r"] < 0.3) else (radii[j] + sp["r"]) * 0.75)
                    if math.hypot(coords[j][0] - x, coords[j][1] - y) < lim:
                        ok = False
                        break
            if ok:
                for q in new:
                    if math.hypot(q[0] - x, q[1] - y) < sp["r"] * 0.9:
                        ok = False
                        break
            if ok:
                new.append((x, y, sp["r"], sp["name"] in BIG))
        placed += new
        print(f"  {sp['name']}: {len(new)} (target {target})")
        if not new:
            continue
        xy = np.array([[q[0], q[1]] for q in new])
        z, nrm, spread = ground(m, tree, xy[:, 0], xy[:, 1])
        for (x, y), zz, nn, sp_ in zip(xy, z, nrm, spread):
            if sp_ > 0.35 + 0.1 * sp["r"]:   # tufts at different heights around it (terrace edge, wall foot): no reliable ground
                continue
            s = rng.uniform(*sp["scale"])
            v = int(rng.integers(0, 2))
            s *= V2_SCALE if v else 1.0
            out.append((sp["name"], v, frame(nn, rng.uniform(0, 2 * math.pi), s),
                        np.array([x, y, zz - sp["sink"]]), sp["range"]))
    return out, area


def templates(n=64):
    names = [t[0] for t in TEMPLATE_MIX]
    p = np.array([t[1] for t in TEMPLATE_MIX])
    out = []
    for _ in range(n):
        k = rng.choice([0, 1, 2, 3], p=[0.15, 0.5, 0.3, 0.05])
        items = []
        for _ in range(k):
            name, v = names[rng.choice(len(names), p=p / p.sum())], int(rng.integers(0, 2))
            items.append((name, v, rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), rng.uniform(0, 2 * math.pi),
                          rng.uniform(0.75, 1.1) * BASE.get(name, 1.0) * (V2_SCALE if v else 1.0)))
        out.append(items)
    return out


def pack_maps(src_dir, dst):
    dst.mkdir(parents=True, exist_ok=True)
    img = Image.open(src_dir / "plant_albedo.png").convert("RGBA").resize((TEX, TEX), Image.LANCZOS)
    rgba = bleed_colour(np.asarray(img, np.float32) / 255.0)
    write(dst / "albedo.ac1t", FMT_BC3, [encode(l, "bc3") for l in mip_chain(rgba, "alpha")], TEX, TEX)
    img = Image.open(src_dir / "plant_normal.png").convert("RGB").resize((TEX, TEX), Image.LANCZOS)
    n = np.asarray(img, np.float32) / 127.5 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    data = []
    for l in mip_chain(n, "mean"):
        l = l / np.maximum(np.linalg.norm(l, axis=-1, keepdims=True), 1e-6)
        data.append(encode(octahedral(l), "bc5"))
    write(dst / "normal.ac1t", FMT_BC5, data, TEX, TEX)
    img = Image.open(src_dir / "plant_roughness.png").convert("L").resize((TEX, TEX), Image.LANCZOS)
    g = np.asarray(img, np.float32)[..., None] / 255.0
    write(dst / "roughness.ac1t", FMT_BC5, [encode(l, "bc5") for l in mip_chain(g, "mean")], TEX, TEX)


def mesh_list():
    """(folder, obj) per mesh index: two game variants per plant, the rebuilt grass once (used for both variants)."""
    meshes = []
    for sp in SPECIES:
        if sp["name"] == "grass":
            meshes += [("grass", "mesh.obj")] * 2
        else:
            meshes += [(sp["name"], f"{sp['name']}_v{v}_game.obj") for v in (1, 2)]
    return meshes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", action="store_true", help="(re)pack the plants' maps and copy the game meshes")
    ap.add_argument("--preview", action="store_true", help="render one planned area in Blender")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    meshes = mesh_list()
    if args.pack:
        for sp in SPECIES:
            dst = OUT / sp["name"]
            if sp["name"] == "grass":
                dst.mkdir(parents=True, exist_ok=True)
                for f in ("mesh.obj", "albedo.ac1t", "normal.ac1t", "roughness.ac1t"):
                    shutil.copy(GRASS / f, dst / f)
                continue
            pack_maps(PLANTS / sp["name"], dst)
            for v in (1, 2):
                shutil.copy(PLANTS / sp["name"] / f"{sp['name']}_v{v}_game.obj", dst)
            print("packed", sp["name"])
    index = {}
    for i, (folder, obj) in enumerate(meshes):
        index.setdefault(folder, []).append(i)
    with open(OUT / "plants.txt", "w") as f:
        for folder, obj in meshes:
            f.write(f"{folder} {obj}\n")
    m = tufts()
    removed, _ = bushes()
    (OUT.parent / "removed.txt").write_text("\n".join(removed) + "\n")
    plants, area = plan(m)
    with open(OUT / "plan.txt", "w") as f:
        for t in m:
            f.write(f"T {t[0, 3]:.3f} {t[1, 3]:.3f} {t[2, 3]:.3f}\n")
        for name, v, rot, p, rng_ in plants:
            mat = np.concatenate([rot, p[:, None]], 1)
            f.write(f"P {index[name][v]} {rng_:.0f} " + " ".join(f"{x:.5f}" for x in mat.ravel()) + "\n")
        for k, tpl in enumerate(templates()):
            for name, v, dx, dy, yaw, s in tpl:
                f.write(f"F {k} {index[name][v]} {dx:.3f} {dy:.3f} {yaw:.4f} {s:.3f}\n")
    print(f"{len(plants)} plants on {area:.0f} m2 ({len(plants) / area:.2f}/m2) -> {OUT}")
    preview_map(m, plants)
    if args.preview:
        preview_area(plants)


COLOURS = {"grass": (120, 170, 60), "9_Hordeum_murinum": (200, 190, 110), "8_Chenopodium_murale": (60, 120, 60),
           "6_Peganum_harmala": (90, 150, 110), "4_FIORI": (240, 220, 60), "5_FIORI": (250, 160, 40),
           "7_Foeniculum_vulgare": (170, 230, 120), "10_Sarcopoterium_spinosum": (150, 90, 60),
           "11_Sarcopoterium_spinosum": (170, 110, 70), "12_Thymbra_capitata": (190, 120, 200),
           "13_Lycium_europaeum": (30, 80, 30), "14_ALBERELLO": (20, 60, 90), "15_Rosa_damascena": (240, 90, 140)}


def preview_map(m, plants):
    from PIL import ImageDraw
    pos = m[:, :2, 3]
    lo, hi = pos.min(0) - 2, pos.max(0) + 2
    s = 3000 / max(hi - lo)
    img = Image.new("RGB", (int((hi - lo)[0] * s), int((hi - lo)[1] * s)), (235, 228, 210))
    d = ImageDraw.Draw(img)
    px = lambda x, y: ((x - lo[0]) * s, (hi[1] - y) * s)  # noqa: E731
    for x, y in pos:
        a, b = px(x, y)
        d.ellipse([a - REACH * s, b - REACH * s, a + REACH * s, b + REACH * s], fill=(214, 204, 180))
    radius = {sp["name"]: sp["r"] for sp in SPECIES}
    for name, v, rot, p, _ in sorted(plants, key=lambda q: -radius[q[0]]):
        a, b = px(p[0], p[1])
        r = max(radius[name] * s * float(np.linalg.norm(rot[:, 0])), 1.5)
        d.ellipse([a - r, b - r, a + r, b + r], fill=COLOURS[name])
    img.save(PLANTS / "_review" / "scatter_map.png")


def preview_area(plants):
    """The densest 14 x 14 m area rendered in Blender with the game meshes."""
    xy = np.array([p[3][:2] for p in plants])
    big = np.array([p[0] in BIG or p[0].startswith(("10_", "11_", "12_", "7_")) for p in plants])
    tree = cKDTree(xy)
    score = np.array([len(tree.query_ball_point(q, 7)) + 25 * big[tree.query_ball_point(q, 7)].sum() for q in xy[::7]])
    c = xy[::7][score.argmax()]
    sel = [p for p in plants if abs(p[3][0] - c[0]) < 7 and abs(p[3][1] - c[1]) < 7]
    lines = []
    for name, v, rot, p, _ in sel:
        if name == "grass":
            tex = ROOT / "assets" / "vegetation" / "grass_modern"
            obj = tex / "grass_modern.obj"
        else:
            tex = PLANTS / name
            obj = tex / f"{name}_v{v + 1}_game.obj"
        mat = np.concatenate([rot, (p - np.array([c[0], c[1], 0]))[:, None]], 1)
        lines.append(f"{obj}|{tex}|" + " ".join(f"{x:.5f}" for x in mat.ravel()))
    zs = [float(p[3][2]) for p in sel]
    (PLANTS / "_work" / "scatter_area.txt").write_text(f"{min(zs):.3f}\n" + "\n".join(lines))
    blender = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
    subprocess.run([blender, "--background", "--factory-startup", "--python",
                    str(ROOT / "tools" / "blender" / "scatter_preview.py")], check=True)


if __name__ == "__main__":
    main()
