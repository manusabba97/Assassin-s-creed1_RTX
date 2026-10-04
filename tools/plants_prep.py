"""Cleans the AI texture sheets of the reference plants (ASSASSINS CREED 1/piante/<n>_<name>_AI.jpg) into game atlases.

For each sheet: mask panels (white shapes on black) are blanked, the sprites are cut out of the grey / checkerboard
background with BiRefNet (MIT, local weights in assets/plants/_work/models/BiRefNet), the colour is upscaled x4 with
Real-ESRGAN x4plus (BSD, local), the mask is upsampled and sharpened, colour is bled under the transparent texels, and
the sprites are listed (connected components) with a numbered preview for choosing leaves / flowers / twigs.
Everything runs locally (CUDA).

  python tools/plants_prep.py [--only N ...]
Output: assets/plants/<n>_<name>/atlas.png (RGBA), sprites.json, sprites_preview.jpg
"""

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import torch
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "assets" / "plants" / "_work"
sys.path.insert(0, str(WORK / "pylib"))
SRC = ROOT.parent / "ASSASSINS CREED 1" / "piante"
OUT = ROOT / "assets" / "plants"
DEV = "cuda"


def load_birefnet():
    from transformers import AutoModelForImageSegmentation
    model = AutoModelForImageSegmentation.from_pretrained(str(WORK / "models" / "BiRefNet"), trust_remote_code=True)
    return model.to(DEV).eval().half()


def load_esrgan():
    import spandrel
    return spandrel.ModelLoader().load_from_file(str(WORK / "models" / "RealESRGAN_x4plus.pth")).to(DEV).eval()


def blank_mask_panels(rgb):
    """AI sheets sometimes carry their own (coarse) mask panels: white shapes on a black rectangle. Fill those
    rectangles with the sheet's background grey so they are not cut out as sprites."""
    v = rgb.max(axis=2)
    dark = (v < 30).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(cv2.morphologyEx(dark, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8)))
    h, w = v.shape
    bg = np.median(np.concatenate([rgb[:8].reshape(-1, 3), rgb[-8:].reshape(-1, 3)]), axis=0)
    out = rgb.copy()
    for i in range(1, n):
        x, y, bw, bh, area = st[i]
        if area > 0.01 * h * w and area > 0.35 * bw * bh:  # a solid dark block, not a dark leaf
            out[y:y + bh, x:x + bw] = bg
    return out


@torch.no_grad()
def cutout(model, rgb):
    h, w = rgb.shape[:2]
    x = cv2.resize(rgb, (1024, 1024), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0
    x = (x - [0.485, 0.456, 0.406]) / [0.229, 0.224, 0.225]
    t = torch.from_numpy(x.transpose(2, 0, 1)).unsqueeze(0).to(DEV).half()
    pred = model(t)[-1].sigmoid()[0, 0].float().cpu().numpy()
    return cv2.resize(pred, (w, h), interpolation=cv2.INTER_CUBIC).clip(0, 1)


def is_checkerboard(rgb):
    """Fake-transparency sheets (grey / white squares) vary along the border; flat grey sheets do not."""
    border = np.concatenate([rgb[:6].reshape(-1, 3), rgb[-6:].reshape(-1, 3),
                             rgb[:, :6].reshape(-1, 3), rgb[:, -6:].reshape(-1, 3)]).astype(np.float32)
    return border.mean(axis=1).std() > 8.0


def keyed_alpha(rgb):
    """Flat-background sheets: alpha from the Lab distance to a smooth background field. BiRefNet keeps only the
    main subject of a multi-sprite sheet (13 Lycium: 1 of ~60 sprites) and leaves grey halos around awns."""
    lab = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    border = np.concatenate([lab[:6].reshape(-1, 3), lab[-6:].reshape(-1, 3)])
    dist0 = np.linalg.norm(lab - np.median(border, axis=0), axis=2)
    obj = (dist0 > 10).astype(np.uint8)
    # background field: inpaint the objects on a small copy, so slow gradients / vignetting are followed
    s = 256.0 / max(rgb.shape[:2])
    small = cv2.resize(rgb, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
    m = cv2.dilate(cv2.resize(obj, (small.shape[1], small.shape[0]), interpolation=cv2.INTER_NEAREST),
                   np.ones((5, 5), np.uint8))
    bg = cv2.inpaint(small, m * 255, 9, cv2.INPAINT_TELEA)
    bg = cv2.GaussianBlur(cv2.resize(bg, (rgb.shape[1], rgb.shape[0]), interpolation=cv2.INTER_CUBIC), (0, 0), 6)
    dist = np.linalg.norm(lab - cv2.cvtColor(bg, cv2.COLOR_RGB2LAB).astype(np.float32), axis=2)
    a = np.clip((dist - 5.0) / 13.0, 0, 1)
    # drop isolated noise specks
    keep = cv2.morphologyEx((a > 0.5).astype(np.uint8), cv2.MORPH_OPEN, np.ones((2, 2), np.uint8))
    n, lab_id, st, _ = cv2.connectedComponentsWithStats(cv2.dilate(keep, np.ones((3, 3), np.uint8)))
    small_ids = np.where(st[:, cv2.CC_STAT_AREA] < 12)[0]
    a[np.isin(lab_id, small_ids)] = 0
    return a


COMFY_INPUT = Path(r"C:\Users\manu\AppData\Local\Comfy-Desktop\ComfyUI-Shared\input")
COMFY_OUTPUT = Path(r"C:\Users\manu\AppData\Local\Comfy-Desktop\ComfyUI-Shared\output")


def comfy(server, path, payload=None):
    import urllib.request
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(server + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def upscale_seedvr2(server, rgb, name):
    """x4 with SeedVR2 7B int8 in the local ComfyUI (user's choice 2026-10-03), the same nodes as the PBR workflow
    (tools/pbr_workflow_api.json group 66): Lanczos x4, SeedVR2 preprocess, tiled VAE, 1-step sampler, LAB colour
    correction."""
    import time
    import uuid
    src = f"ac1_plants_{name}.png"
    Image.fromarray(rgb).save(COMFY_INPUT / src)
    prompt = {
        "1": {"class_type": "LoadImage", "inputs": {"image": src}},
        "57": {"class_type": "ResizeImageMaskNode", "inputs": {"resize_type": "scale by multiplier",
                                                               "resize_type.multiplier": 4, "scale_method": "lanczos",
                                                               "input": ["1", 0]}},
        "58": {"class_type": "SeedVR2Preprocess", "inputs": {"resized_images": ["57", 0]}},
        "51": {"class_type": "VAELoader", "inputs": {"vae_name": "seedvr2_ema_vae_fp16.safetensors"}},
        "52": {"class_type": "UNETLoader", "inputs": {"unet_name": "seedvr2_7b_int8_convrot.safetensors",
                                                     "weight_dtype": "default"}},
        "48": {"class_type": "VAEEncodeTiled", "inputs": {"tile_size": 512, "overlap": 128, "temporal_size": 4096,
                                                         "temporal_overlap": 8, "pixels": ["58", 0], "vae": ["51", 0]}},
        "61": {"class_type": "SeedVR2Conditioning", "inputs": {"model": ["52", 0], "vae_conditioning": ["48", 0]}},
        "54": {"class_type": "KSampler", "inputs": {"seed": 959948902156062, "steps": 1, "cfg": 1,
                                                   "sampler_name": "euler", "scheduler": "simple", "denoise": 1,
                                                   "model": ["52", 0], "positive": ["61", 0], "negative": ["61", 1],
                                                   "latent_image": ["48", 0]}},
        "55": {"class_type": "VAEDecodeTiled", "inputs": {"tile_size": 512, "overlap": 128, "temporal_size": 4096,
                                                         "temporal_overlap": 8, "samples": ["54", 0], "vae": ["51", 0]}},
        "59": {"class_type": "SeedVR2PostProcessing", "inputs": {"color_correction_method": "lab",
                                                                "images": ["55", 0],
                                                                "original_resized_images": ["57", 0]}},
        "90": {"class_type": "SaveImage", "inputs": {"images": ["59", 0], "filename_prefix": f"ac1_plants/{name}"}},
    }
    pid = comfy(server, "/prompt", {"prompt": prompt, "client_id": str(uuid.uuid4())})["prompt_id"]
    while True:
        time.sleep(3)
        hist = comfy(server, f"/history/{pid}").get(pid)
        if hist and hist.get("status", {}).get("completed"):
            break
        if hist and hist.get("status", {}).get("status_str") == "error":
            raise RuntimeError(f"SeedVR2 failed on {name}: {hist['status']}")
    img = hist["outputs"]["90"]["images"][0]
    out = np.asarray(Image.open(COMFY_OUTPUT / img["subfolder"] / img["filename"]).convert("RGB"))
    if out.shape[:2] != (rgb.shape[0] * 4, rgb.shape[1] * 4):
        out = cv2.resize(out, (rgb.shape[1] * 4, rgb.shape[0] * 4), interpolation=cv2.INTER_LANCZOS4)
    return out


@torch.no_grad()
def upscale(model, rgb, tile=512, pad=16):
    h, w = rgb.shape[:2]
    x = torch.from_numpy(rgb.astype(np.float32).transpose(2, 0, 1) / 255.0).unsqueeze(0).to(DEV)
    out = torch.zeros((1, 3, h * 4, w * 4), device=DEV)
    for y0 in range(0, h, tile):
        for x0 in range(0, w, tile):
            ya, xa = max(y0 - pad, 0), max(x0 - pad, 0)
            yb, xb = min(y0 + tile + pad, h), min(x0 + tile + pad, w)
            r = model(x[:, :, ya:yb, xa:xb])
            oy, ox = (y0 - ya) * 4, (x0 - xa) * 4
            th, tw = (min(y0 + tile, h) - y0) * 4, (min(x0 + tile, w) - x0) * 4
            out[:, :, y0 * 4:y0 * 4 + th, x0 * 4:x0 * 4 + tw] = r[:, :, oy:oy + th, ox:ox + tw]
    return (out[0].clamp(0, 1).cpu().numpy().transpose(1, 2, 0) * 255).round().astype(np.uint8)


def bleed(rgb, alpha):
    sys.path.insert(0, str(ROOT / "tools"))
    from pbr_pack import bleed_colour
    rgba = np.dstack([rgb.astype(np.float32) / 255.0, alpha])
    return (bleed_colour(rgba, 0.5)[..., :3] * 255).round().astype(np.uint8)


def sprites(alpha, rgb, min_area_frac=0.0004):
    m = (alpha > 0.5).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(cv2.dilate(m, np.ones((5, 5), np.uint8)))
    h, w = alpha.shape
    out = []
    for i in range(1, n):
        x, y, bw, bh, area = st[i]
        if area < min_area_frac * h * w:
            continue
        sel = (lab[y:y + bh, x:x + bw] == i) & (m[y:y + bh, x:x + bw] > 0)
        c = rgb[y:y + bh, x:x + bw][sel].mean(axis=0) if sel.any() else np.zeros(3)
        out.append({"id": len(out), "box": [int(x), int(y), int(bw), int(bh)], "fill": round(float(sel.mean()), 3),
                    "colour": [int(v) for v in c]})
    return out


def preview(rgb, alpha, sp, path):
    bg = np.full_like(rgb, 70)
    comp = (rgb * alpha[..., None] + bg * (1 - alpha[..., None])).astype(np.uint8)
    im = Image.fromarray(comp)
    im.thumbnail((1600, 1600))
    s = im.width / rgb.shape[1]
    d = ImageDraw.Draw(im)
    for p in sp:
        x, y, w, h = [v * s for v in p["box"]]
        d.rectangle([x, y, x + w, y + h], outline=(255, 60, 60))
        d.text((x + 2, y + 1), str(p["id"]), fill=(255, 255, 0))
    im.save(path, quality=85)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", type=int)
    ap.add_argument("--server", default="http://127.0.0.1:8189", help="ComfyUI for SeedVR2 ('' = Real-ESRGAN)")
    args = ap.parse_args()
    seg = load_birefnet()
    up = None if args.server else load_esrgan()
    for f in sorted(SRC.glob("*_AI.jpg"), key=lambda p: int(p.name.split("_")[0])):
        n = int(f.name.split("_")[0])
        if args.only and n not in args.only:
            continue
        name = f.name[:-len("_AI.jpg")]
        out = OUT / name
        out.mkdir(parents=True, exist_ok=True)
        rgb = blank_mask_panels(np.asarray(Image.open(f).convert("RGB")))
        checker = is_checkerboard(rgb)
        alpha = cutout(seg, rgb) if checker else keyed_alpha(rgb)
        big = upscale_seedvr2(args.server, rgb, name) if args.server else upscale(up, rgb)
        a4 = cv2.resize(alpha, (big.shape[1], big.shape[0]), interpolation=cv2.INTER_CUBIC)
        a4 = np.clip((a4 - 0.35) / 0.3, 0, 1)  # sharpen the upsampled edge
        # the x4 upscale rings dark along edges against the grey: pull the mask in by ~2.5 px
        a4 = cv2.erode(a4.astype(np.float32), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)))
        big = bleed(big, a4)
        Image.fromarray(np.dstack([big, (a4 * 255).round().astype(np.uint8)]), "RGBA").save(out / "atlas.png")
        sp = sprites(a4, big)
        json.dump({"source": f.name, "size": [big.shape[1], big.shape[0]], "sprites": sp},
                  open(out / "sprites.json", "w"), indent=1)
        preview(big, a4, sp, out / "sprites_preview.jpg")
        print(f"{name}: {big.shape[1]}x{big.shape[0]}, {len(sp)} sprites")


if __name__ == "__main__":
    main()
