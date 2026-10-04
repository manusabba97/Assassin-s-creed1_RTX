"""Cutout of the plant sheets with a dedicated AI node (user 2026-10-03: "let a dedicated AI node do it"):
ComfyUI-RMBG's BiRefNet (RMBG) node, model BiRefNet-HR-matting with refine_foreground (foreground colour
estimation: no grey halo on soft edges such as awns), run on the SeedVR2 x4 sheet (ComfyUI output ac1_plants/).

The sheet holds many separate pieces and a matting model keeps only the main subject of an image, so each piece is
found first (colour key against the grey background, tools/plants_prep.py keyed_alpha, at the original resolution),
cropped with a margin from the x4 sheet and matted on its own; the crops are merged back (max alpha).
Writes atlas.png, sprites.json and sprites_preview.jpg like plants_prep.py, with the same sprite ids (boxes from the
same key), so plant.json's choices stay valid.

  python tools/plants_matte.py [--only N ...] [--server http://127.0.0.1:8189]
"""

import argparse
import json
import time
import uuid
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

import plants_prep as prep

COMFY_IN, COMFY_OUT = prep.COMFY_INPUT, prep.COMFY_OUTPUT


def matte(server, crop_rgb, tag):
    name = f"ac1_matte_{tag}.png"
    Image.fromarray(crop_rgb).save(COMFY_IN / name)
    prompt = {
        "1": {"class_type": "LoadImage", "inputs": {"image": name}},
        "2": {"class_type": "BiRefNetRMBG", "inputs": {"image": ["1", 0], "model": "BiRefNet-HR-matting",
                                                     "sensitivity": 1.0, "mask_blur": 0, "mask_offset": 0,
                                                     "invert_output": False, "refine_foreground": True,
                                                     "unload_model": False, "background": "Alpha",
                                                     "background_color": "#222222"}},
        "3": {"class_type": "SaveImage", "inputs": {"images": ["2", 0], "filename_prefix": f"ac1_matte/{tag}"}},
    }
    pid = prep.comfy(server, "/prompt", {"prompt": prompt, "client_id": str(uuid.uuid4())})["prompt_id"]
    while True:
        time.sleep(0.4)
        hist = prep.comfy(server, f"/history/{pid}").get(pid)
        if hist and hist.get("status", {}).get("completed"):
            break
        if hist and hist.get("status", {}).get("status_str") == "error":
            raise RuntimeError(f"matting failed on {tag}: {hist['status']}")
    img = hist["outputs"]["3"]["images"][0]
    out = np.asarray(Image.open(COMFY_OUT / img["subfolder"] / img["filename"]).convert("RGBA"))
    return out


def pieces(alpha, margin=0.08):
    """Boxes of the separate pieces at the original resolution (merged when they overlap after the margin)."""
    m = (alpha > 0.5).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(cv2.dilate(m, np.ones((5, 5), np.uint8)))
    h, w = alpha.shape
    boxes = []
    for i in range(1, n):
        x, y, bw, bh, area = st[i]
        if area < 0.0003 * h * w:
            continue
        p = int(max(bw, bh) * margin) + 4
        boxes.append([max(x - p, 0), max(y - p, 0), min(x + bw + p, w), min(y + bh + p, h)])
    return boxes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", type=int)
    ap.add_argument("--server", default="http://127.0.0.1:8189")
    args = ap.parse_args()
    for f in sorted(prep.SRC.glob("*_AI.jpg"), key=lambda p: int(p.name.split("_")[0])):
        n = int(f.name.split("_")[0])
        if args.only and n not in args.only:
            continue
        name = f.name[:-len("_AI.jpg")]
        out = prep.OUT / name
        rgb = prep.blank_mask_panels(np.asarray(Image.open(f).convert("RGB")))
        # fake-transparency (checkerboard) sheets cannot be colour keyed: find the pieces with BiRefNet instead
        key = prep.cutout(prep.load_birefnet(), rgb) if prep.is_checkerboard(rgb) else prep.keyed_alpha(rgb)
        big = np.asarray(Image.open(COMFY_OUT / "ac1_plants" / f"{name}_00001_.png").convert("RGB"))
        if big.shape[:2] != (rgb.shape[0] * 4, rgb.shape[1] * 4):  # SeedVR2 pads; keep exactly x4 (same sprite ids)
            big = cv2.resize(big, (rgb.shape[1] * 4, rgb.shape[0] * 4), interpolation=cv2.INTER_LANCZOS4)
        sx, sy = big.shape[1] / rgb.shape[1], big.shape[0] / rgb.shape[0]
        alpha = np.zeros(big.shape[:2], np.float32)
        colour = big.astype(np.float32).copy()
        boxes = pieces(key)
        for k, (x0, y0, x1, y1) in enumerate(boxes):
            X0, Y0, X1, Y1 = int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy)
            crop = big[Y0:Y1, X0:X1]
            res = matte(args.server, crop, f"{n}_{k}")
            if res.shape[:2] != crop.shape[:2]:
                res = cv2.resize(res, (crop.shape[1], crop.shape[0]), interpolation=cv2.INTER_CUBIC)
            a = res[..., 3].astype(np.float32) / 255.0
            # only what this crop's piece covers: drop matte that strays into neighbouring pieces' area
            keep = cv2.resize(key[y0:y1, x0:x1], (crop.shape[1], crop.shape[0]), interpolation=cv2.INTER_LINEAR)
            a *= cv2.dilate((keep > 0.05).astype(np.uint8), np.ones((15, 15), np.uint8)).astype(np.float32)
            region = alpha[Y0:Y1, X0:X1]
            better = a > region
            colour[Y0:Y1, X0:X1][better] = res[..., :3][better]
            region[better] = a[better]
        rgb_out = prep.bleed(colour.round().astype(np.uint8), alpha)
        Image.fromarray(np.dstack([rgb_out, (alpha * 255).round().astype(np.uint8)]), "RGBA").save(out / "atlas.png")
        # sprite boxes from the key exactly as plants_prep.py computed them, so plant.json's ids stay valid
        a4 = cv2.resize(key, (big.shape[1], big.shape[0]), interpolation=cv2.INTER_CUBIC)
        a4 = np.clip((a4 - 0.35) / 0.3, 0, 1)
        a4 = cv2.erode(a4.astype(np.float32), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)))
        sp = prep.sprites(a4, rgb_out)
        json.dump({"source": f.name, "size": [big.shape[1], big.shape[0]], "sprites": sp},
                  open(out / "sprites.json", "w"), indent=1)
        prep.preview(rgb_out, alpha, sp, out / "sprites_preview.jpg")
        print(f"{name}: {len(boxes)} pieces matted, {len(sp)} sprites", flush=True)


if __name__ == "__main__":
    main()
