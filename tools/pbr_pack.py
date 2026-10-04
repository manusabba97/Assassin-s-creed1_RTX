"""Packs the workflow's PNG maps into GPU-ready block-compressed files the mod uploads as they are (no decoding in
the 32-bit game process: a 4096x4096 set took ~450 MB uncompressed and the game hung while loading).

For each pbr/maps/<hash>/<variant>/, the newest <hash>_<map>_NNNNN.png becomes <hash>_<map>.ac1t:
  header "AC1T", uint32 format (remixapi_Format value), width, height, mip levels; then the mip chain, level 0 first.
  albedo     BC1 (opaque) / BC3 (with alpha)        box-filtered mips
  normal     BC5, Remix unsigned octahedral          vectors averaged per mip, then encoded
             (same conversion as remix.cpp encodeNormalTexel: OpenGL / engine convention, green flipped)
  roughness  BC5 (value in R)                        box-filtered mips
  metallic   BC5 (value in R)                        box-filtered mips
  height     BC5 (value in R)                        max-filtered mips (QuadtreePOM uses maximum mips)
Mips stop at 4x4 (one BC block). Run with ComfyUI's venv (Pillow >= 11 has the BCn encoder):
  python tools/pbr_pack.py [--variant x4] [--only HASH ...]
"""

import argparse
import re
import struct
from pathlib import Path

import numpy as np
from PIL import Image

PBR = Path(__file__).resolve().parent.parent / "pbr"
FMT_BC1_RGBA, FMT_BC3, FMT_BC5 = 133, 135, 139  # remix_c.h remixapi_Format


def newest(folder, name, m):
    best, idx = None, -1
    for f in folder.glob(f"{name}_{m}_*.png"):
        g = re.fullmatch(rf"{re.escape(name)}_{m}_(\d+)\.png", f.name)
        if g and int(g.group(1)) > idx:
            best, idx = f, int(g.group(1))
    return best


def mip_chain(a, reduce):
    """a: float32 HxWxC; reduce 'mean' or 'max' over 2x2."""
    levels = [a]
    while min(levels[-1].shape[:2]) > 4:
        x = levels[-1]
        h, w = x.shape[0] // 2 * 2, x.shape[1] // 2 * 2
        q = x[:h, :w].reshape(h // 2, 2, w // 2, 2, -1)
        if reduce == "max":
            levels.append(q.max(axis=(1, 3)))
        elif reduce == "alpha":
            # RGBA: colour averaged weighted by alpha (premultiplied), so transparent texels do not tint the edges
            a = q[..., 3:4]
            wsum = a.sum(axis=(1, 3))
            rgb = np.where(wsum > 1e-4, (q[..., :3] * a).sum(axis=(1, 3)) / np.maximum(wsum, 1e-4),
                           q[..., :3].mean(axis=(1, 3)))
            levels.append(np.concatenate([rgb, wsum / 4.0], axis=-1))
        else:
            levels.append(q.mean(axis=(1, 3)))
    return levels


def bleed_colour(rgba, threshold=0.5):
    """Colour under (nearly) transparent texels replaced by the colour of the nearest visible ones (push-pull over a
    weight pyramid). The workflow returns black there (dump colour (107,103,70) -> (8,7,4) on decal ABB1E590), which
    filtering, mips and BC3 pulled into the visible edges as dark fringes ("flakes" on floor decals, 2026-10-03)."""
    w = (rgba[..., 3] >= threshold).astype(np.float32)
    if w.min() > 0 or w.max() == 0:
        return rgba
    pyr = [(rgba[..., :3] * w[..., None], w)]
    while min(pyr[-1][1].shape) > 1:
        c, ww = pyr[-1]
        h, wd = c.shape[0] // 2 * 2, c.shape[1] // 2 * 2
        if h == 0 or wd == 0:
            break
        c2 = c[:h, :wd].reshape(h // 2, 2, wd // 2, 2, 3).sum(axis=(1, 3))
        w2 = ww[:h, :wd].reshape(h // 2, 2, wd // 2, 2).sum(axis=(1, 3))
        pyr.append((c2, w2))
    fill = pyr[-1][0] / np.maximum(pyr[-1][1], 1e-6)[..., None]
    for c, ww in reversed(pyr[:-1]):
        up = np.repeat(np.repeat(fill, 2, axis=0), 2, axis=1)
        pad_h, pad_w = c.shape[0] - up.shape[0], c.shape[1] - up.shape[1]
        if pad_h > 0 or pad_w > 0:
            up = np.pad(up, ((0, max(pad_h, 0)), (0, max(pad_w, 0)), (0, 0)), mode="edge")
        up = up[:c.shape[0], :c.shape[1]]
        own = c / np.maximum(ww, 1e-6)[..., None]
        fill = np.where(ww[..., None] > 0, own, up)
    out = rgba.copy()
    out[..., :3] = np.where(w[..., None] > 0, rgba[..., :3], fill)
    return out


def encode(level, kind):
    u8 = np.clip(np.round(level * 255.0), 0, 255).astype(np.uint8)
    if kind == "bc1":
        return Image.fromarray(u8[..., :3], "RGB").tobytes("bcn", 1)
    if kind == "bc3":
        return Image.fromarray(u8, "RGBA").tobytes("bcn", 3)
    rg = np.zeros(u8.shape[:2] + (3,), np.uint8)  # BC5 from R, G
    rg[..., 0] = u8[..., 0]
    rg[..., 1] = u8[..., 1] if u8.shape[-1] > 1 else 0
    return Image.fromarray(rg, "RGB").tobytes("bcn", 5)


def octahedral(n):
    """Unit vectors (OpenGL, green up) -> Remix unsigned octahedral (x, y) in [0, 1]."""
    x, y, z = n[..., 0], -n[..., 1], np.maximum(n[..., 2], 0.0)
    s = np.abs(x) + np.abs(y) + z
    s = np.where(s > 0, s, 1.0)
    px, py = x / s, y / s
    return np.stack([(px + py) * 0.5 + 0.5, (px - py) * 0.5 + 0.5], axis=-1)


def write(path, fmt, levels_bytes, w, h):
    with open(path, "wb") as f:
        f.write(b"AC1T" + struct.pack("<4I", fmt, w, h, len(levels_bytes)))
        for b in levels_bytes:
            f.write(b)


# Material classes (2026-10-03, user: "make them photoreal, full freedom"). The VLM's material name picks the class
# by the earliest keyword in it ("lime plaster on mud brick" -> plaster). Per class:
#   rough     perceptual roughness range the workflow's [roughness_min, roughness_max] output is remapped to (Remix:
#             white = rough); stone and earth stay matte, leather and metal get a sheen, cloth and thatch fully matte
#   relief    relief height as a fraction of one texture repeat (POM depth in texture units: a texture covering 8 m of
#             cliff shows big rocks and gets ~24 cm, one covering 30 cm of a prop gets ~1 cm)
#   pom       parallax on by default (cloth, leather, metal, skin, foliage: normal map only)
MATERIAL_CLASSES = [
    # class        keywords                                                                   rough        relief  pom
    ("plaster",    ("plaster", "stucco", "whitewash"),                                         (0.70, 0.95), 0.008, True),
    ("thatch",     ("thatch", "straw", "reed", "hay"),                                         (0.82, 1.00), 0.020, True),
    ("cloth",      ("linen", "cloth", "wool", "carpet", "rug", "textile", "fabric", "banner",
                    "tunic", "trousers", "sheepskin", "silk", "cotton", "canvas", "rope"),     (0.85, 1.00), 0.000, False),
    ("leather",    ("leather", "hide", "cuirass", "harness", "strap"),                         (0.45, 0.80), 0.000, False),
    ("metal",      ("iron", "bronze", "gold", "chainmail", "metal", "steel", "coin", "medallion",
                    "sword", "copper", "brass", "silver"),                                     (0.30, 0.65), 0.000, False),
    ("skin",       ("skin",),                                                                  (0.45, 0.70), 0.000, False),
    ("foliage",    ("leaf", "leaves", "ivy", "moss", "vegetation", "grass", "foliage"),        (0.50, 0.85), 0.000, False),
    ("wood",       ("wood", "plank", "timber", "beam", "stump", "root", "bark", "log"),        (0.55, 0.90), 0.010, True),
    ("earth",      ("earth", "soil", "dirt", "sand", "mud", "pebble", "gravel", "dust"),       (0.75, 1.00), 0.012, True),
    ("cliff",      ("cliff", "rock", "boulder", "crag"),                                       (0.60, 0.95), 0.030, True),
    ("rubble",     ("rubble", "fieldstone", "cobble"),                                         (0.55, 0.92), 0.022, True),
    ("brick",      ("brick", "tile"),                                                          (0.60, 0.92), 0.012, True),
    ("stone",      ("stone", "sandstone", "marble", "slab", "column", "masonry", "block"),     (0.55, 0.92), 0.015, True),
]
DEFAULT_CLASS = ("other", (), (0.60, 0.90), 0.012, True)
# Metal class without a metallic map: weathered / oxidised iron and bronze read as partly metallic (rust and dirt
# are dielectric), so a constant below 1 (the mod uses it as Remix metallicConstant).
METAL_CONSTANT = 0.7


def classify(material):
    text = (material or "").lower()
    best, pos, best_len = DEFAULT_CLASS, len(text) + 1, 0
    for cls in MATERIAL_CLASSES:
        for k in cls[1]:
            i = text.find(k)
            # earliest keyword wins; at the same position the longer one ("sandstone" over "sand")
            if 0 <= i and (i < pos or (i == pos and len(k) > best_len)):
                best, pos, best_len = cls, i, len(k)
    return best


def newest_json(folder, name):
    best, idx = None, -1
    for f in folder.glob(f"{name}_material_*.json"):
        g = re.fullmatch(rf"{re.escape(name)}_material_(\d+)\.json", f.name)
        if g and int(g.group(1)) > idx:
            best, idx = f, int(g.group(1))
    return best


def tune(folder, name):
    """Once per workflow run (material JSON "tuned"): roughness PNG remapped to the class range, and the JSON gets
    class, relief_uv, height_std, metallic_constant and pom_default (class allows it, albedo >= 512 px i.e. source
    >= 128 px, height not flat). The albedo's alpha does not count: opaque engine textures often keep a mask there, and
    alpha-tested surfaces get no POM in the mod anyway."""
    import json
    mat_path = newest_json(folder, name)
    if not mat_path:
        return "no material JSON"
    mat = json.loads(mat_path.read_text(encoding="utf-8"))
    if mat.get("tuned"):
        if "metallic_constant" not in mat:  # added after the first tuning pass
            mat["metallic_constant"] = METAL_CONSTANT if mat.get("class") == "metal" else 0.0
            mat_path.write_text(json.dumps(mat, indent=1), encoding="utf-8")
        return f"class {mat.get('class')} (already tuned)"
    params = mat.setdefault("params", {})
    cls_name, _, (clo, chi), relief, pom = classify(params.get("material", ""))
    lo, hi = params.get("roughness_min", 0.6), params.get("roughness_max", 0.9)
    r_path = newest(folder, name, "roughness")
    if r_path:
        r = np.asarray(Image.open(r_path).convert("L"), np.float32) / 255.0
        r = clo + np.clip((r - lo) / max(hi - lo, 1e-3), 0.0, 1.0) * (chi - clo)
        Image.fromarray(np.clip(np.rint(r * 255.0), 0, 255).astype(np.uint8)).save(r_path, compress_level=4)
    params["roughness_min"], params["roughness_max"] = clo, chi
    h_path = newest(folder, name, "height")
    std = float(np.asarray(Image.open(h_path).convert("L"), np.float32).std() / 255.0) if h_path else 0.0
    a_path = newest(folder, name, "albedo")
    alpha, width = False, 0
    if a_path:
        img = Image.open(a_path)
        width = min(img.size)
        alpha = img.mode in ("RGBA", "LA") and np.asarray(img.getchannel("A")).min() < 255
    mat.update({"tuned": 1, "class": cls_name, "relief_uv": relief, "height_std": round(std, 4),
                "metallic_constant": METAL_CONSTANT if cls_name == "metal" else 0.0,
                "pom_default": bool(pom and relief > 0 and width >= 512 and std >= 0.05)})
    mat_path.write_text(json.dumps(mat, indent=1), encoding="utf-8")
    return f"class {cls_name}, roughness {clo}-{chi}, relief {relief}, pom {mat['pom_default']}"


def pack(folder, name):
    out = [tune(folder, name)]
    run = newest(folder, name, "albedo")
    for m in ("albedo", "normal", "roughness", "metallic", "height"):
        src = newest(folder, name, m)
        # every map of one workflow run has the same NNNNN; older runs' maps are not mixed in
        if src and run and src.name[-9:] != run.name[-9:]:
            src = None
        if not src:
            stale = folder / f"{name}_{m}.ac1t"
            if stale.exists():
                stale.unlink()
                out.append(f"{m} absent in run {run.name[-9:-4] if run else '?'}: removed {stale.name}")
            continue
        img = Image.open(src)
        if m == "albedo":
            rgba = np.asarray(img.convert("RGBA"), np.float32) / 255.0
            alpha = rgba[..., 3].min() < 1.0
            if alpha:
                rgba = bleed_colour(rgba)
            levels = mip_chain(rgba, "alpha" if alpha else "mean")
            fmt, data = (FMT_BC3, [encode(l, "bc3") for l in levels]) if alpha else \
                        (FMT_BC1_RGBA, [encode(l, "bc1") for l in levels])
        elif m == "normal":
            n = np.asarray(img.convert("RGB"), np.float32) / 127.5 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            levels = mip_chain(n, "mean")
            data = []
            for l in levels:
                l = l / np.maximum(np.linalg.norm(l, axis=-1, keepdims=True), 1e-6)
                data.append(encode(octahedral(l), "bc5"))
            fmt = FMT_BC5
        else:
            g = np.asarray(img.convert("L"), np.float32)[..., None] / 255.0
            levels = mip_chain(g, "max" if m == "height" else "mean")
            fmt, data = FMT_BC5, [encode(l, "bc5") for l in levels]
        write(folder / f"{name}_{m}.ac1t", fmt, data, img.width, img.height)
        out.append(f"{m} {img.width}x{img.height} {len(data)} mips {sum(map(len, data)) / 2**20:.1f} MB")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="x4")
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    for d in sorted((PBR / "maps").iterdir()):
        if args.only and d.name not in args.only:
            continue
        folder = d / args.variant
        if folder.is_dir():
            print(d.name, "; ".join(pack(folder, d.name)))


if __name__ == "__main__":
    main()
