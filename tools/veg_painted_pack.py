"""Packs the rebuilt plants for the in-game vegetation editor (mod remix.cpp "Vegetation editor", runtime panel M ->
Vegetazione): pbr/vegetation/painted/<species>/ albedo / normal / roughness .ac1t (2048) + the game meshes
(plant_builder.py ... game), and painted/plants.txt ("<folder> <obj> <name>" per mesh). placed.txt (the painted
plants) is written by the mod and left alone.
Colours 10 % duller than the sheets (user 2026-10-03): albedo saturation x0.9.
  python tools/veg_painted_pack.py
"""

import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pbr_pack import FMT_BC3, FMT_BC5, PBR, bleed_colour, encode, mip_chain, octahedral, write  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PLANTS = ROOT / "assets" / "plants"
GRASS = ROOT / "assets" / "vegetation" / "grass_modern"
OUT = PBR / "vegetation" / "painted"
TEX = 2048
SATURATION = 0.9

# ground plants (the wall plants 1-3 grow out of walls and are left out)
SPECIES = [
    ("grass", "Erba"),
    ("9_Hordeum_murinum", "Orzo selvatico (Hordeum)"),
    ("8_Chenopodium_murale", "Farinello (Chenopodium)"),
    ("6_Peganum_harmala", "Ruta siriana (Peganum)"),
    ("4_FIORI", "Malva"),
    ("5_FIORI", "Enula (Dittrichia)"),
    ("7_Foeniculum_vulgare", "Finocchio selvatico"),
    ("12_Thymbra_capitata", "Timo (Thymbra)"),
    ("10_Sarcopoterium_spinosum", "Spinaporci"),
    ("11_Sarcopoterium_spinosum", "Spinaporci con frutti"),
    ("13_Lycium_europaeum", "Spina santa (Lycium)"),
    ("15_Rosa_damascena", "Rosa damascena"),
    ("14_ALBERELLO", "Alloro giovane"),
]


def pack_maps(albedo, normal, rough, dst):
    img = Image.open(albedo).convert("RGBA").resize((TEX, TEX), Image.LANCZOS)
    rgba = np.asarray(img, np.float32) / 255.0
    luma = (rgba[..., :3] * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(-1, keepdims=True)
    rgba[..., :3] = np.clip(luma + (rgba[..., :3] - luma) * SATURATION, 0.0, 1.0)
    rgba = bleed_colour(rgba)
    write(dst / "albedo.ac1t", FMT_BC3, [encode(l, "bc3") for l in mip_chain(rgba, "alpha")], TEX, TEX)
    img = Image.open(normal).convert("RGB").resize((TEX, TEX), Image.LANCZOS)
    n = np.asarray(img, np.float32) / 127.5 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    data = []
    for l in mip_chain(n, "mean"):
        l = l / np.maximum(np.linalg.norm(l, axis=-1, keepdims=True), 1e-6)
        data.append(encode(octahedral(l), "bc5"))
    write(dst / "normal.ac1t", FMT_BC5, data, TEX, TEX)
    img = Image.open(rough).convert("L").resize((TEX, TEX), Image.LANCZOS)
    g = np.asarray(img, np.float32)[..., None] / 255.0
    write(dst / "roughness.ac1t", FMT_BC5, [encode(l, "bc5") for l in mip_chain(g, "mean")], TEX, TEX)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    lines = []
    for folder, name in SPECIES:
        dst = OUT / folder
        dst.mkdir(exist_ok=True)
        label = name.replace(" ", "_")
        if folder == "grass":
            pack_maps(GRASS / "grass_albedo.png", GRASS / "grass_normal.png", GRASS / "grass_roughness.png", dst)
            shutil.copy(GRASS / "grass_modern.obj", dst / "mesh.obj")
            lines.append(f"{folder} mesh.obj {label}")
        else:
            src = PLANTS / folder
            pack_maps(src / "plant_albedo.png", src / "plant_normal.png", src / "plant_roughness.png", dst)
            for v in (1, 2):
                shutil.copy(src / f"{folder}_v{v}_game.obj", dst)
                lines.append(f"{folder} {folder}_v{v}_game.obj {label}")
        print("packed", folder)
    (OUT / "plants.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
