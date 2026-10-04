"""Applies new workflow defaults to maps already generated, so they match a fresh run of the workflow:
  normal     X/Y scaled by --normal (re-normalised), as AC1_MatchNormal strength
  roughness  extra contrast --rough-contrast around the middle of [roughness_min, roughness_max] (the pivot of
             AC1_RoughnessFusion's contrast after its remap), clipped to that range: equal to running the workflow
             with contrast multiplied by the same factor
  height     remix_height_strength in the material JSON multiplied by --height
Hand edits (<hash>_edit.json) keep their look: a normal_strength the editor changed is divided by --normal.
The newest PNG of each map is rewritten; then run tools/pbr_pack.py to rebuild the .ac1t files.
  python tools/pbr_defaults_bake.py [--variant x4] [--backup DIR] [--only HASH ...]
"""

import argparse
import json
import re
import shutil
from pathlib import Path

import numpy as np
from PIL import Image

from pbr_pack import PBR, newest


def newest_json(folder, name):
    best, idx = None, -1
    for f in folder.glob(f"{name}_material_*.json"):
        g = re.fullmatch(rf"{re.escape(name)}_material_(\d+)\.json", f.name)
        if g and int(g.group(1)) > idx:
            best, idx = f, int(g.group(1))
    return best


def save_png(path, a):
    Image.fromarray(np.clip(np.rint(a * 255.0), 0, 255).astype(np.uint8)).save(path, compress_level=4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="x4")
    ap.add_argument("--normal", type=float, default=1.2)
    ap.add_argument("--rough-contrast", type=float, default=1.15)
    ap.add_argument("--height", type=float, default=3.0)
    ap.add_argument("--backup", default=str(PBR / "maps_pre_defaults"))
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()

    for tex in sorted((PBR / "maps").iterdir()):
        folder = tex / args.variant
        name = tex.name
        if not folder.is_dir() or (args.only and name not in args.only):
            continue
        mat_path = newest_json(folder, name)
        files = [p for p in (newest(folder, name, "normal"), newest(folder, name, "roughness"), mat_path,
                             folder / f"{name}_edit.json") if p and p.exists()]
        backup = Path(args.backup) / name / args.variant
        backup.mkdir(parents=True, exist_ok=True)
        for p in files:
            shutil.copy2(p, backup / p.name)

        mat = json.loads(mat_path.read_text(encoding="utf-8")) if mat_path else {}
        params = mat.get("params", {})
        lo, hi = params.get("roughness_min", 0.6), params.get("roughness_max", 0.9)

        n_path = newest(folder, name, "normal")
        if n_path:
            n = np.asarray(Image.open(n_path).convert("RGB"), np.float32) / 255.0 * 2.0 - 1.0
            n[..., :2] *= args.normal
            n[..., 2] = np.maximum(n[..., 2], 0.0)
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            save_png(n_path, n * 0.5 + 0.5)

        r_path = newest(folder, name, "roughness")
        if r_path:
            img = Image.open(r_path)
            r = np.asarray(img.convert("RGB"), np.float32) / 255.0
            mid = (lo + hi) * 0.5
            save_png(r_path, np.clip(mid + (r - mid) * args.rough_contrast, lo, hi))

        if mat_path:
            mat["remix_height_strength"] = round(mat.get("remix_height_strength", 0.02) * args.height, 6)
            if "params" in mat:
                mat["params"]["remix_height_strength"] = mat["remix_height_strength"]
            mat_path.write_text(json.dumps(mat, indent=1), encoding="utf-8")

        edit = folder / f"{name}_edit.json"
        note = ""
        if edit.exists():
            text = edit.read_text(encoding="utf-8")
            m = re.search(r'"normal_strength":\s*([-+0-9.eE]+)', text)
            if m and abs(float(m.group(1)) - 1.0) > 1e-4:
                ns = float(m.group(1)) / args.normal
                text = text[:m.start(1)] + f"{ns:.4f}" + text[m.end(1):]
                edit.write_text(text, encoding="utf-8")
                note = f", edit normal_strength {m.group(1)} -> {ns:.4f}"
        print(f"{name}: normal x{args.normal}, roughness contrast x{args.rough_contrast} in [{lo}, {hi}], "
              f"height {mat.get('remix_height_strength')}{note}")


if __name__ == "__main__":
    main()
