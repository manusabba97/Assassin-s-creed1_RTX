"""One comparison image per plant: reference photo | variant 1 | variant 2 (Blender previews), with names, sizes and
triangle counts, in assets/plants/_review/compare_<plant>.jpg.

  python tools/plants_compare.py
"""

import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
PLANTS = ROOT / "assets" / "plants"
SRC = ROOT.parent / "ASSASSINS CREED 1" / "piante"
H = 640


def tris(obj):
    if not obj.exists():
        return 0
    n = 0
    for line in open(obj, encoding="utf-8", errors="ignore"):
        if line.startswith("f "):
            n += len(line.split()) - 3
    return n


def fit(im, h=H):
    im = im.convert("RGB")
    return im.resize((int(im.width * h / im.height), h), Image.LANCZOS)


def main():
    for d in sorted(p for p in PLANTS.iterdir() if p.is_dir() and not p.name.startswith("_")):
        photo = SRC / f"{d.name}.jpg"
        panels = [("foto", fit(Image.open(photo)))]
        for v in (1, 2):
            pv = d / f"preview_v{v}.png"
            if pv.exists():
                panels.append((f"v{v}: {tris(d / f'{d.name}_v{v}.obj')} triangoli", fit(Image.open(pv))))
        width = sum(p.width for _, p in panels) + 8 * (len(panels) - 1)
        sheet = Image.new("RGB", (width, H + 30), (25, 25, 25))
        x = 0
        dr = ImageDraw.Draw(sheet)
        for label, p in panels:
            sheet.paste(p, (x, 30))
            dr.text((x + 6, 8), label, fill=(255, 255, 255))
            x += p.width + 8
        dr.text((width - 300, 8), d.name, fill=(255, 220, 120))
        sheet.save(PLANTS / "_review" / f"compare_{d.name}.jpg", quality=88)
        print(d.name, len(panels) - 1, "variants")


if __name__ == "__main__":
    main()
