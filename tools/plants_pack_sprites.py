"""Repacks the sprites a plant uses (assets/plants/<plant>/plant.json "roles") from the cleaned sheet (atlas.png,
sprites.json) into one compact game atlas: each sprite copied with ITS OWN mask only (sheet boxes overlap), shelf
packed, colour bled under transparency; a stem patch (a twig sprite stretched, or the spec's stem colour) is added
so the whole plant uses one material. Writes plant_albedo.png (RGBA), plant_normal.png (from the albedo's detail),
plant_roughness.png and plant_uv.json (per sprite: uv rect, aspect, role).

  python tools/plants_pack_sprites.py <plant folder name> [...]
"""

import json
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
PLANTS = ROOT / "assets" / "plants"
sys.path.insert(0, str(ROOT / "tools"))
from pbr_pack import bleed_colour  # noqa: E402

ATLAS = 4096
PAD = 8


def labels(alpha):
    """Same components as plants_prep.sprites (dilated 5x5), so the sprite ids match."""
    m = (alpha > 0.5).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(cv2.dilate(m, np.ones((5, 5), np.uint8)))
    return lab, st


def sprite_label(lab, box):
    x, y, w, h = box
    sub = lab[y:y + h, x:x + w]
    ids, counts = np.unique(sub[sub > 0], return_counts=True)
    return int(ids[np.argmax(counts)])


def shelf_pack(sizes, width):
    """sizes: list of (w, h); returns positions and the used height."""
    order = sorted(range(len(sizes)), key=lambda i: -sizes[i][1])
    pos = [None] * len(sizes)
    x = y = shelf = 0
    for i in order:
        w, h = sizes[i]
        if x + w + PAD > width:
            x, y, shelf = 0, y + shelf + PAD, 0
        pos[i] = (x, y)
        x += w + PAD
        shelf = max(shelf, h)
    return pos, y + shelf


def normal_from_albedo(rgba, strength=2.5):
    """Fine relief from the albedo's luminance (veins, needles, bark): Sobel of a lightly blurred height, OpenGL."""
    l = (rgba[..., :3] @ [0.2126, 0.7152, 0.0722]).astype(np.float32)
    l = cv2.GaussianBlur(l, (0, 0), 1.2)
    gx = cv2.Sobel(l, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(l, cv2.CV_32F, 0, 1, ksize=3)
    n = np.dstack([-gx * strength, gy * strength, np.ones_like(l)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return (n * 0.5 + 0.5).clip(0, 1)


def pack(plant):
    d = PLANTS / plant
    spec = json.load(open(d / "plant.json", encoding="utf-8"))
    sheet = json.load(open(d / "sprites.json", encoding="utf-8"))
    img = np.asarray(Image.open(d / "atlas.png").convert("RGBA"), np.float32) / 255.0
    lab, _ = labels(img[..., 3])
    boxes = {s["id"]: s["box"] for s in sheet["sprites"]}
    items = []  # (key, role, rgba crop)
    for role, ids in spec["roles"].items():
        for sid in ids:
            x, y, w, h = boxes[sid]
            own = (lab[y:y + h, x:x + w] == sprite_label(lab, boxes[sid]))
            crop = img[y:y + h, x:x + w].copy()
            crop[..., 3] *= own
            items.append((f"{role}:{sid}", role, crop))
    # stem patch: the first twig sprite rotated so its length runs along v, or a flat colour
    stem_src = spec.get("stem_sprite")
    if stem_src is not None:
        x, y, w, h = boxes[stem_src]
        crop = img[y:y + h, x:x + w].copy()
        if w > h:
            crop = np.ascontiguousarray(np.rot90(crop))
        # keep the central strip of the twig (its bark), opaque
        cw = crop.shape[1]
        crop = crop[:, cw // 2 - max(cw // 8, 2): cw // 2 + max(cw // 8, 2)]
        crop[..., 3] = 1.0
        crop = cv2.resize(crop, (64, 512), interpolation=cv2.INTER_AREA)
    else:
        c = np.array(spec.get("stem_colour", [90, 110, 60]), np.float32) / 255.0
        noise = np.random.default_rng(1).normal(0, 0.03, (512, 64, 1)).astype(np.float32)
        crop = np.dstack([np.clip(c + noise, 0, 1), np.ones((512, 64), np.float32)])
    items.append(("stem", "stem", crop))
    # scale so everything fits in ATLAS x ATLAS
    scale = 1.0
    for _ in range(30):
        sizes = [(max(int(c.shape[1] * scale), 4), max(int(c.shape[0] * scale), 4)) for _, _, c in items]
        pos, used = shelf_pack(sizes, ATLAS)
        if used <= ATLAS:
            break
        scale *= 0.92
    atlas = np.zeros((ATLAS, ATLAS, 4), np.float32)
    uv = {}
    for (key, role, crop), (w, h), (px, py) in zip(items, sizes, pos):
        r = cv2.resize(crop, (w, h), interpolation=cv2.INTER_AREA)
        atlas[py:py + h, px:px + w] = r
        uv[key] = {"role": role, "rect": [px / ATLAS, py / ATLAS, (px + w) / ATLAS, (py + h) / ATLAS],
                   "aspect": w / h, "px": [w, h]}
    atlas = bleed_colour(atlas, 0.5)
    Image.fromarray((atlas * 255).round().astype(np.uint8), "RGBA").save(d / "plant_albedo.png")
    Image.fromarray((normal_from_albedo(atlas) * 255).round().astype(np.uint8)).save(d / "plant_normal.png")
    rough = np.full((ATLAS, ATLAS), spec.get("roughness_leaf", 0.55), np.float32)
    for key, u in uv.items():
        if u["role"] in ("stem", "twig"):
            x0, y0, x1, y1 = [int(v * ATLAS) for v in u["rect"]]
            rough[y0:y1, x0:x1] = spec.get("roughness_stem", 0.85)
    Image.fromarray((rough * 255).round().astype(np.uint8)).save(d / "plant_roughness.png")
    json.dump(uv, open(d / "plant_uv.json", "w"), indent=1)
    print(f"{plant}: {len(items)} sprites packed at scale {scale:.2f}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        pack(p)
