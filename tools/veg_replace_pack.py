"""Packs a rebuilt vegetation asset for the mod's replacement of an exported mesh (remix.cpp vegReplacement):
pbr/vegetation/replace/<geometry id>/ mesh.obj + albedo.ac1t (BC3, colour bled under transparency, alpha-weighted
mips) + normal.ac1t (BC5 octahedral) + roughness.ac1t (BC5).
  python tools/veg_replace_pack.py <geometry id> <obj> <albedo.png> <normal.png> <roughness.png>
"""

import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from pbr_pack import FMT_BC3, FMT_BC5, PBR, bleed_colour, encode, mip_chain, octahedral, write


def main():
    gid, obj, albedo, normal, rough = sys.argv[1:6]
    out = PBR / "vegetation" / "replace" / gid
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy(obj, out / "mesh.obj")
    img = Image.open(albedo)
    rgba = bleed_colour(np.asarray(img.convert("RGBA"), np.float32) / 255.0)
    levels = mip_chain(rgba, "alpha")
    write(out / "albedo.ac1t", FMT_BC3, [encode(l, "bc3") for l in levels], img.width, img.height)
    img = Image.open(normal)
    n = np.asarray(img.convert("RGB"), np.float32) / 127.5 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    data = []
    for l in mip_chain(n, "mean"):
        l = l / np.maximum(np.linalg.norm(l, axis=-1, keepdims=True), 1e-6)
        data.append(encode(octahedral(l), "bc5"))
    write(out / "normal.ac1t", FMT_BC5, data, img.width, img.height)
    img = Image.open(rough)
    g = np.asarray(img.convert("L"), np.float32)[..., None] / 255.0
    write(out / "roughness.ac1t", FMT_BC5, [encode(l, "bc5") for l in mip_chain(g, "mean")], img.width, img.height)
    print(f"{out}: mesh.obj, albedo/normal/roughness.ac1t")


if __name__ == "__main__":
    main()
