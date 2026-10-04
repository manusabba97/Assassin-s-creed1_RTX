"""Per-plant choices (sprite ids from assets/plants/<plant>/sprites_preview.jpg, chosen by looking at each sheet) and
growth parameters; writes assets/plants/<plant>/plant.json and a review image (chosen sprites framed in green with
their role, the rest dimmed) to assets/plants/_review/<plant>.jpg.

Roles: branch (a stem with its leaves / flowers, used as a card along the modelled stems), sprig (small leafy twig),
leaf, flower, bud, fruit, spike (grass ear), blade (grass leaf), frond (fennel), thorn, twig (dry wood card), litter.
Excluded on purpose: whole-plant / whole-tree renders (flat, wrong scale), other species the AI put on the sheet
(6: caper flowers and nettle; 7: nettle and grass; 11: broad leaves of another shrub), text, roots and cut stems.
Sizes: real plants (metres), from the photos and the species.

  python tools/plants_specs.py
"""

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance

ROOT = Path(__file__).resolve().parent.parent
PLANTS = ROOT / "assets" / "plants"

SPECS = {
    "1_Caper_on_Anafi": dict(
        flowers_per_stem=4, sizes={"branch": [0.35, 0.5], "sprig": [0.12, 0.2], "twig": [0.1, 0.18]}, twig_count=12, leaf_density=130, card_every=0.09, species="Capparis spinosa", form="trailing", height=0.45, width=1.4, stems=9,
        roles=dict(branch=[0], sprig=[1], leaf=[2, 3, 4], flower=[5, 6, 10], bud=[8, 9, 14],
                   twig=[11, 17, 20, 22]),
        stem_colour=[120, 140, 95], leaf_size=[0.035, 0.055], flower_size=[0.06, 0.08]),
    "2_Parietaria_judaica": dict(
        sizes={"sprig": [0.06, 0.1]}, leaf_density=110, species="Parietaria judaica", form="wall", height=0.45, width=0.6, stems=14,
        roles=dict(sprig=[0, 1, 2, 3], leaf=[6, 8, 9, 10, 11, 12]),
        stem_sprite=4, leaf_size=[0.025, 0.05]),
    "3_Homot_jerusalaim": dict(
        rosette_count=26, species="wall rosette (pinnatifid leaves)", form="wall_rosette", height=0.25, width=0.4, stems=18,
        roles=dict(frond=[6]),
        stem_colour=[110, 130, 70], leaf_size=[0.08, 0.14]),
    "4_FIORI": dict(
        flowers_per_stem=10, sizes={"sprig": [0.15, 0.25]}, species="Malva sylvestris", form="upright", height=0.85, width=0.7, stems=6,
        roles=dict(sprig=[2, 4, 7], leaf=[8, 9, 10], flower=[5, 6]),
        stem_colour=[105, 130, 70], leaf_size=[0.08, 0.14], flower_size=[0.045, 0.055]),
    "5_FIORI": dict(
        flowers_per_stem=10, sizes={"branch": [0.3, 0.45]}, leaf_density=70, card_every=0.1, stem_radius=0.004, species="Dittrichia viscosa", form="upright", height=1.0, width=0.6, stems=7, spread=[2, 14], taper=0.75, tip_t=[0.5, 0.98], droop=0.02, wiggle=0.05,
        roles=dict(branch=[1, 2, 3], leaf=[4, 5, 6, 10, 12, 13, 14, 15], leaf_dry=[19, 20, 21],
                   flower=[7, 16], bud=[9, 11, 17]),
        stem_colour=[110, 120, 75], leaf_size=[0.04, 0.07], flower_size=[0.02, 0.028]),
    "6_Peganum_harmala": dict(
        flowers_per_stem=3, species="Peganum harmala", form="herb", height=0.38, width=0.3, stems=7, spread=[4, 28], droop=0.08, stem_radius=0.0018, stem_colour=[105, 125, 70], sizes={"branch": [0.1, 0.16]}, leaf_density=60, card_every=0.16,
        # the sheet's sprigs 7/8/11/14/29/33 have round caper-like leaves; Peganum's are narrow and finely divided,
        # as on the three small whole plants 0-2, used here as branch clusters
        roles=dict(branch=[0, 1, 2], leaf=[30, 31, 32, 34, 35, 39, 40, 41],
                   flower=[16, 17], bud=[22, 23, 25, 36]),
        leaf_size=[0.03, 0.06], flower_size=[0.03, 0.035]),
    "7_Foeniculum_vulgare": dict(
        sizes={"branch": [0.5, 0.8], "frond": [0.25, 0.45], "frond_thin": [0.25, 0.4], "sprig": [0.12, 0.2]}, card_every=0.06, stem_radius=0.006, species="Foeniculum vulgare", form="upright", height=1.4, width=0.9, stems=14,
        roles=dict(branch=[2, 7, 18], frond=[0, 1, 8, 9, 25, 26, 27], frond_thin=[3, 10, 28, 29],
                   sprig=[40, 42, 43, 45]),
        stem_colour=[125, 145, 95], leaf_size=[0.15, 0.3]),
    "8_Chenopodium_murale": dict(
        sizes={"branch": [0.18, 0.3], "spike": [0.05, 0.1], "sprig": [0.1, 0.15]}, leaf_density=55, species="Chenopodium murale", form="upright", height=0.6, width=0.55, stems=6,
        roles=dict(branch=[20, 26, 39], leaf=[15, 16, 18, 19, 21, 22, 23, 25, 28, 29, 30, 33, 34, 37, 40, 41, 42,
                                              44, 45, 46],
                   leaf_dry=[17, 24, 31, 43], spike=[1, 2, 4, 5, 6, 7], sprig=[48, 49, 50, 51]),
        stem_colour=[110, 135, 80], leaf_size=[0.06, 0.11]),
    "9_Hordeum_murinum": dict(
        species="Hordeum murinum", form="tuft", height=0.4, width=0.45, stems=60,
        roles=dict(spike=[0, 3, 4, 5, 6, 10, 18, 20, 23, 24, 26], spike_dry=[1, 2, 7, 21, 22, 25],
                   branch=[11, 12, 13, 14, 15, 16, 17],
                   blade=[29, 30, 31, 32, 33, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 47], litter=[46]),
        stem_colour=[150, 155, 100]),
    "10_Sarcopoterium_spinosum": dict(
        sizes={"sprig": [0.07, 0.12], "twig": [0.08, 0.15]}, species="Sarcopoterium spinosum", form="cushion", height=0.5, width=0.85, stems=26,
        roles=dict(sprig=[2, 4, 26, 27, 29, 35, 36, 37], twig=[1, 6, 8, 11],
                   leaf=[5, 10, 12, 13, 14, 20, 22, 23, 24, 25], thorn=[15, 16, 17, 28, 30, 31]),
        stem_colour=[175, 170, 165], leaf_size=[0.015, 0.03]),
    "11_Sarcopoterium_spinosum": dict(
        sizes={"branch": [0.15, 0.25], "sprig": [0.05, 0.08], "twig": [0.08, 0.15]}, fruit_size=[0.012, 0.02], species="Sarcopoterium spinosum (fruiting)", form="cushion", height=0.55, width=0.8, stems=24,
        roles=dict(branch=[40, 41], sprig=[36, 37, 38], leaf=[15, 19, 20, 23, 24, 25, 26, 27],
                   fruit=[28, 29, 30, 31, 32, 35]),
        stem_colour=[180, 175, 168], leaf_size=[0.015, 0.03]),
    "12_Thymbra_capitata": dict(
        flowers_per_stem=3, sizes={"branch": [0.12, 0.2]}, species="Thymbra capitata", form="dome", height=0.45, width=0.85, stems=30,
        roles=dict(branch=[7, 8, 9, 10, 11, 12, 13, 15], flower=[30, 35, 60, 73],
                   leaf=[18, 19, 20, 21, 23, 24, 25, 26, 27, 38, 39, 42, 44, 45, 46, 47, 48, 49], twig=[14, 16, 17]),
        stem_colour=[120, 95, 75], leaf_size=[0.006, 0.01], flower_size=[0.03, 0.045]),
    "13_Lycium_europaeum": dict(
        flowers_per_stem=3, sizes={"branch": [0.35, 0.6], "sprig": [0.1, 0.18], "twig": [0.2, 0.4]}, fruit_size=[0.008, 0.012], flower_size=[0.012, 0.018], leaf_density=70, card_every=0.1, side_branches=[5, 8], species="Lycium europaeum", form="dome", height=1.3, width=1.6, stems=22, shell_density=220,
        roles=dict(branch=[0, 1, 2], sprig=[9, 11, 12, 13, 14, 19, 20, 21, 22, 23, 24, 25, 26, 27],
                   leaf=[28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43, 44, 45, 50, 51, 52, 55, 56, 57, 58, 59,
                         60, 61, 62, 63],
                   thorn=[4, 5, 6, 7, 8, 10, 15, 16], flower=[40, 41], twig=[65, 68, 85]),
        stem_colour=[150, 140, 125], leaf_size=[0.015, 0.03]),
    "14_ALBERELLO": dict(
        sizes={"branch": [0.25, 0.4]}, spread=[3, 12], leaf_density=55, card_every=0.12, species="Laurus nobilis (young)", form="leader", height=1.6, width=0.7, stems=1, nodes=16,
        roles=dict(branch=[4, 5, 7, 8, 11, 12, 13, 14, 21],
                   leaf=[39, 40, 41, 42, 43, 44, 45, 46, 47, 49, 52, 53, 54, 55, 57, 59, 60, 61, 64],
                   twig=[25, 27, 29, 30]),
        stem_sprite=20, leaf_size=[0.06, 0.1]),
    "15_Rosa_damascena": dict(
        flowers_per_stem=3, sizes={"branch": [0.25, 0.4]}, leaf_density=45, species="Rosa damascena", form="rose", height=1.2, width=1.3, stems=9, bloom_3d=True, stem_radius=0.007,
        roles=dict(branch=[3, 19, 20], flower=[1], bud=[2, 4, 5, 7, 21], leaf_compound=[22, 23, 24, 25, 49, 50],
                   leaf=[26, 27, 28, 29, 30, 31, 32, 33, 34, 41, 42, 43, 44, 45], stem=[35, 37, 38]),
        stem_sprite=13, leaf_size=[0.03, 0.05], flower_size=[0.08, 0.1]),
}

ROLE_COLOURS = {"leaf": (80, 220, 80), "flower": (255, 120, 220), "bud": (255, 180, 120), "fruit": (230, 60, 60),
                "branch": (80, 200, 255), "sprig": (120, 255, 200), "twig": (200, 160, 110), "thorn": (230, 230, 230),
                "spike": (240, 220, 90), "blade": (150, 230, 100), "frond": (100, 220, 160), "litter": (190, 170, 120)}


def review(plant, spec):
    d = PLANTS / plant
    sheet = json.load(open(d / "sprites.json", encoding="utf-8"))
    img = Image.open(d / "atlas.png").convert("RGBA")
    bg = Image.new("RGBA", img.size, (60, 60, 60, 255))
    comp = Image.alpha_composite(bg, img).convert("RGB")
    dim = ImageEnhance.Brightness(comp).enhance(0.28)
    chosen = {sid: role for role, ids in spec["roles"].items() for sid in ids}
    if spec.get("stem_sprite") is not None:
        chosen[spec["stem_sprite"]] = "stem"
    mask = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(mask)
    boxes = {s["id"]: s["box"] for s in sheet["sprites"]}
    for sid in chosen:
        x, y, w, h = boxes[sid]
        md.rectangle([x, y, x + w, y + h], fill=255)
    out = Image.composite(comp, dim, mask)
    out.thumbnail((1600, 1600))
    s = out.width / img.width
    dr = ImageDraw.Draw(out)
    for sid, role in chosen.items():
        x, y, w, h = [v * s for v in boxes[sid]]
        col = ROLE_COLOURS.get(role.split("_")[0], (255, 255, 255))
        dr.rectangle([x, y, x + w, y + h], outline=col, width=2)
        dr.text((x + 3, y + 2), f"{sid} {role}", fill=col)
    dr.rectangle([0, 0, out.width, 22], fill=(20, 20, 20))
    dr.text((6, 5), f"{plant}  -  {spec['species']}  -  {spec['form']}, {spec['height']} m x {spec['width']} m", fill=(255, 255, 255))
    rv = PLANTS / "_review"
    rv.mkdir(exist_ok=True)
    out.save(rv / f"{plant}.jpg", quality=86)


def main():
    for plant, spec in SPECS.items():
        json.dump(spec, open(PLANTS / plant / "plant.json", "w", encoding="utf-8"), indent=1)
        review(plant, spec)
        print(plant, sum(len(v) for v in spec["roles"].values()), "sprites")


if __name__ == "__main__":
    main()
