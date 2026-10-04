"""AC1 height lab: nodes to compare height-map methods side by side (everything local).

AC1_DeepBumpNormals   AI normal map from a colour image (DeepBump deepbump256.onnx, models/deepbump/)
AC1_HeightFinalize    one proposal -> reference size, white = high (auto polarity), normalised, blurred N px
AC1_HeightHybrid      coarse shape from one height, fine relief from another (Laplacian pyramid)
AC1_HeightLabSheet    labelled grid of the proposals (height + lit relief) and one PNG per proposal
"""

import os

import cv2
import numpy as np
import torch

import folder_paths

from . import (_normalize as normalize, _pyramid_fuse as pyramid_fuse, _resize_like as resize_like,
               _rgb_to_image as rgb_to_image, _to_gray as to_gray, _to_image as to_image)

LABELS = [
    "01 Qwen-Edit 2509 da albedo (VLM)",
    "02 Qwen-Edit 2509 da normal di gioco (VLM)",
    "03 Flux.2 Klein 4B da albedo (VLM)",
    "04 PBRify Height (CC0, Remix)",
    "05 Depth Anything 3",
    "06 Lotus Depth",
    "07 MoGe-2 depth",
    "08 DeepBump normal -> integrata",
    "09 Normal di gioco + forma DA3 (ibrido)",
    "10 PBRify Normal -> integrata",
]

# models/deepbump next to every configured models folder (the install's and the extra paths' shared one)
folder_paths.folder_names_and_paths.setdefault(
    "deepbump", (sorted({os.path.join(os.path.dirname(p.rstrip("\\/")), "deepbump")
                         for p in folder_paths.get_folder_paths("vae")}), {".onnx"}))

_DEEPBUMP = {}


def _deepbump_session(name):
    if name not in _DEEPBUMP:
        import onnxruntime as ort
        ort.disable_telemetry_events()
        _DEEPBUMP[name] = ort.InferenceSession(folder_paths.get_full_path_or_raise("deepbump", name),
                                               providers=["CPUExecutionProvider"])
    return _DEEPBUMP[name]


class AC1_DeepBumpNormals:
    """DeepBump colour -> normals: grey (mean of RGB) in 256 px tiles with wrapped borders, blended with a linear
    ramp over the overlap, each pixel normalised. Runs at most at max_side (the model is 256 px based)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "model": (folder_paths.get_filename_list("deepbump") or ["deepbump256.onnx"],),
                "overlap": (["small", "medium", "large"], {"default": "large"}),
                "max_side": ("INT", {"default": 2048, "min": 256, "max": 8192, "step": 64}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("normal",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, image, model, overlap, max_side):
        img = image[0, :, :, :3].cpu().numpy().astype(np.float32)
        H, W = img.shape[:2]
        k = min(1.0, max_side / max(H, W))
        if k < 1.0:
            img = cv2.resize(img, (round(W * k), round(H * k)), interpolation=cv2.INTER_AREA)
        grey = img.mean(axis=-1)
        tile = 256
        ov = {"small": tile // 6, "medium": tile // 4, "large": tile // 2}[overlap]
        ov -= ov % 2
        stride = tile - ov
        h0, w0 = grey.shape
        ph = (-(h0 - tile) % stride) if h0 > tile else tile - h0
        pw = (-(w0 - tile) % stride) if w0 > tile else tile - w0
        pt, pl = ph // 2 + stride, pw // 2 + stride
        g = np.pad(grey, ((pt, ph - ph // 2 + stride), (pl, pw - pw // 2 + stride)), mode="wrap")
        acc = np.zeros((3,) + g.shape, np.float32)
        wsum = np.zeros(g.shape, np.float32)
        ramp = np.minimum(np.arange(tile) + 1, tile - np.arange(tile)).astype(np.float32)
        ramp = np.minimum(ramp / max(ov, 1), 1.0)
        mask = np.outer(ramp, ramp)
        sess = _deepbump_session(model)
        for y in range(0, g.shape[0] - tile + 1, stride):
            for x in range(0, g.shape[1] - tile + 1, stride):
                t = g[y:y + tile, x:x + tile][None, None]
                pred = sess.run(None, {"input": t.astype(np.float32)})[0][0]
                acc[:, y:y + tile, x:x + tile] += pred * mask
                wsum[y:y + tile, x:x + tile] += mask
        n = acc / np.maximum(wsum, 1e-6)
        n = n[:, pt:pt + h0, pl:pl + w0].transpose(1, 2, 0)
        v = n - 0.5
        v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
        return (rgb_to_image(v * 0.5 + 0.5),)


class AC1_HeightFinalize:
    """Brings one proposal to the reference size (Lanczos), normalises it (0.5-99.5 percentile) and blurs it by
    blur_px (Gaussian, 2 sigma = blur_px). Polarity: auto = white must be high, decided by the sign of the
    correlation with polarity_reference (the height integrated from the game normal map, physically signed);
    without a reference, or when the correlation is weak (|r| < 0.05), the input is kept."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "height": ("IMAGE",),
                "reference": ("IMAGE",),
                "blur_px": ("FLOAT", {"default": 4.0, "min": 0.0, "max": 64.0, "step": 0.5}),
                "polarity": (["auto", "keep", "invert"], {"default": "auto"}),
                "flatten_percent": ("FLOAT", {"default": 0.0, "min": 0.0, "max": 50.0, "step": 0.5,
                                              "tooltip": "removes undulations larger than this % of the short side "
                                                         "(scene-depth models see the texture as a tilted photo)"}),
            },
            "optional": {
                "polarity_reference": ("IMAGE",),
            },
        }

    RETURN_TYPES = ("IMAGE", "STRING")
    RETURN_NAMES = ("height", "note")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, height, reference, blur_px, polarity, flatten_percent=0.0, polarity_reference=None):
        Hh, Ww = reference.shape[1:3]
        x = to_gray(height)
        if x.shape != (Hh, Ww):
            x = cv2.resize(x, (Ww, Hh), interpolation=cv2.INTER_LANCZOS4)
        if flatten_percent > 0:
            sigma = flatten_percent / 100.0 * min(x.shape)
            x = x - cv2.GaussianBlur(x, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)
        x = normalize(x)
        note = "kept"
        if polarity == "invert":
            x, note = 1.0 - x, "inverted"
        elif polarity == "auto" and polarity_reference is not None:
            r = cv2.resize(to_gray(polarity_reference), (256, 256), interpolation=cv2.INTER_AREA)
            s = cv2.resize(x, (256, 256), interpolation=cv2.INTER_AREA)
            corr = float(np.corrcoef(r.ravel(), s.ravel())[0, 1])
            if np.isfinite(corr) and corr < -0.05:
                x, note = 1.0 - x, f"inverted (r={corr:.2f})"
            else:
                note = f"kept (r={corr:.2f})"
        if blur_px >= 0.5:
            rad = int(np.ceil(blur_px))
            x = cv2.GaussianBlur(x, (2 * rad + 1, 2 * rad + 1), blur_px / 2.0, borderType=cv2.BORDER_REFLECT)
            x = normalize(x, 0.0, 100.0)
        return (to_image(x), note)


class AC1_HeightHybrid:
    """Coarse shape from `coarse` (e.g. an AI depth), fine relief from `fine` (e.g. the game normal map
    integrated): Laplacian pyramid, the fine input's share grows toward the finest bands (mean share = fine_weight)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "coarse": ("IMAGE",),
                "fine": ("IMAGE",),
                "fine_weight": ("FLOAT", {"default": 0.5, "min": 0.0, "max": 1.0, "step": 0.05}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("height",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, coarse, fine, fine_weight):
        f = normalize(to_gray(fine))
        c = normalize(resize_like(to_gray(coarse), f))
        return (to_image(normalize(pyramid_fuse(c, f, fine_weight))),)


def _hillshade(x, azimuth=315.0, altitude=45.0, z=8.0):
    gy, gx = np.gradient(x * z * x.shape[0] / 256.0)
    slope = np.pi / 2.0 - np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    az, alt = np.radians(azimuth), np.radians(altitude)
    s = np.sin(alt) * np.sin(slope) + np.cos(alt) * np.cos(slope) * np.cos(az - aspect)
    return np.clip(s, 0.0, 1.0)


class AC1_HeightLabSheet:
    """Comparison sheet: one column per proposal, height on top and the same height lit from the top-left below
    (relief as POM will show it), with its label; the albedo first. Saves each proposal as
    <folder>/<texture>/NN_<name>.png and the sheet as <folder>/<texture>/sheet.png."""

    @classmethod
    def INPUT_TYPES(cls):
        opt = {f"h{i:02}": ("IMAGE",) for i in range(1, 11)}
        opt.update({f"note{i:02}": ("STRING", {"forceInput": True}) for i in range(1, 11)})
        return {
            "required": {
                "albedo": ("IMAGE",),
                "folder": ("STRING", {"default": os.path.join(os.path.expanduser("~"), "Desktop", "AC1_height_lab")}),
                "texture": ("STRING", {"default": "texture"}),
                "cell": ("INT", {"default": 384, "min": 128, "max": 1024, "step": 32}),
                "labels": ("STRING", {"default": "\n".join(LABELS), "multiline": True}),
            },
            "optional": opt,
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("sheet",)
    FUNCTION = "run"
    OUTPUT_NODE = True
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, albedo, folder, texture, cell, labels, **kw):
        from PIL import Image, ImageDraw, ImageFont
        import nodes
        names = [l.strip() for l in labels.splitlines()] + [""] * 10
        out_dir = os.path.join(folder, texture)
        os.makedirs(out_dir, exist_ok=True)
        try:
            font = ImageFont.truetype("arial.ttf", max(12, cell // 22))
        except OSError:
            font = ImageFont.load_default()
        tiles = []
        a = albedo[0, :, :, :3].cpu().numpy()
        tiles.append(("albedo", cv2.resize(a, (cell, cell), interpolation=cv2.INTER_AREA), None))
        for i in range(1, 11):
            img = kw.get(f"h{i:02}")
            if img is None:
                continue
            x = to_gray(img)
            name = names[i - 1] or f"{i:02}"
            safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name)[:60]
            Image.fromarray((np.clip(x, 0, 1) * 65535).astype(np.uint16)).save(os.path.join(out_dir, f"{safe}.png"))
            small = cv2.resize(x, (cell, cell), interpolation=cv2.INTER_AREA)
            note = kw.get(f"note{i:02}") or ""
            tiles.append((name + (f"  [{note}]" if note else ""), np.stack([small] * 3, -1),
                          np.stack([_hillshade(small)] * 3, -1)))
        cols = 6
        rows = (len(tiles) + cols - 1) // cols
        label_h = cell // 5
        sheet = Image.new("RGB", (cols * cell, rows * (2 * cell + label_h)), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for k, (name, top, bottom) in enumerate(tiles):
            x0, y0 = (k % cols) * cell, (k // cols) * (2 * cell + label_h)
            sheet.paste(Image.fromarray((np.clip(top, 0, 1) * 255).astype(np.uint8)), (x0, y0 + label_h))
            if bottom is not None:
                sheet.paste(Image.fromarray((np.clip(bottom, 0, 1) * 255).astype(np.uint8)), (x0, y0 + label_h + cell))
            words, lines, line = name.split(" "), [], ""
            for w in words:
                if draw.textlength((line + " " + w).strip(), font=font) > cell - 8 and line:
                    lines.append(line)
                    line = w
                else:
                    line = (line + " " + w).strip()
            lines.append(line)
            draw.multiline_text((x0 + 4, y0 + 2), "\n".join(lines[:3]), fill=(235, 235, 235), font=font, spacing=2)
        sheet.save(os.path.join(out_dir, "sheet.png"))
        print(f"[AC1-PBR] height lab: {len(tiles) - 1} proposals saved to {out_dir}")
        t = torch.from_numpy(np.asarray(sheet, np.float32) / 255.0).unsqueeze(0)
        preview = nodes.PreviewImage().save_images(t, filename_prefix="ac1_height_lab")
        return {"ui": preview["ui"], "result": (t,)}


MIX_MODES = ["media", "moltiplica", "screen", "overlay", "soft light", "massimo", "minimo", "somma", "differenza",
             "forma A + dettaglio B"]


class AC1_ImageMix:
    """Mixes two images (e.g. two height proposals). B is brought to A's size. factor = amount of the blend over A
    (0 = only A, 1 = full blend). forma A + dettaglio B: coarse bands from A, fine bands from B (Laplacian pyramid,
    factor = mean share of B). normalize stretches the result to 0-1 (heights)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image_a": ("IMAGE",),
                "image_b": ("IMAGE",),
                "mode": (MIX_MODES, {"default": "media"}),
                "factor": ("FLOAT", {"default": 0.5, "min": 0.0, "max": 1.0, "step": 0.01}),
                "normalize": ("BOOLEAN", {"default": True}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, image_a, image_b, mode, factor, normalize):
        a = image_a[0, :, :, :3].cpu().numpy().astype(np.float32)
        b = image_b[0, :, :, :3].cpu().numpy().astype(np.float32)
        if b.shape[:2] != a.shape[:2]:
            b = cv2.resize(b, (a.shape[1], a.shape[0]), interpolation=cv2.INTER_LANCZOS4)
        if mode == "forma A + dettaglio B":
            ga, gb = a.mean(-1), b.mean(-1)
            out = pyramid_fuse(ga, gb, factor)
            out = np.stack([out] * 3, -1)
        else:
            if mode == "media":
                m = b
            elif mode == "moltiplica":
                m = a * b
            elif mode == "screen":
                m = 1.0 - (1.0 - a) * (1.0 - b)
            elif mode == "overlay":
                m = np.where(a < 0.5, 2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b))
            elif mode == "soft light":
                m = (1.0 - 2.0 * b) * a * a + 2.0 * b * a
            elif mode == "massimo":
                m = np.maximum(a, b)
            elif mode == "minimo":
                m = np.minimum(a, b)
            elif mode == "somma":
                m = a + b
            else:  # differenza
                m = np.abs(a - b)
            out = a + (m - a) * factor
        if normalize:
            lo, hi = np.percentile(out, [0.1, 99.9])
            out = (out - lo) / max(hi - lo, 1e-6)
        return (torch.from_numpy(np.clip(out, 0.0, 1.0).astype(np.float32)).unsqueeze(0),)


class AC1_ImageBlur:
    """Blur after the mix. gaussiana: radius px (2 sigma = radius). bilaterale: smooths flat areas, keeps edges
    (stone borders). mediana: removes speckles, keeps edges."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "radius_px": ("FLOAT", {"default": 4.0, "min": 0.0, "max": 128.0, "step": 0.5}),
                "mode": (["gaussiana", "bilaterale", "mediana"], {"default": "gaussiana"}),
                "seamless": ("BOOLEAN", {"default": True, "tooltip": "wrap at the borders (tiling textures)"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, image, radius_px, mode, seamless):
        x = image[0, :, :, :3].cpu().numpy().astype(np.float32)
        if radius_px < 0.5:
            return (image,)
        r = int(np.ceil(radius_px))
        pad = 2 * r + 2
        xp = np.pad(x, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if seamless else "reflect")
        if mode == "gaussiana":
            y = cv2.GaussianBlur(xp, (2 * r + 1, 2 * r + 1), radius_px / 2.0)
        elif mode == "bilaterale":
            y = cv2.bilateralFilter(xp, d=2 * r + 1, sigmaColor=0.1, sigmaSpace=radius_px / 2.0)
        else:
            k = 2 * r + 1 if 2 * r + 1 <= 5 else 5  # float images: OpenCV medianBlur supports ksize 3 or 5
            y = cv2.medianBlur(xp, k)
            for _ in range(max(0, r // 2 - 1)):
                y = cv2.medianBlur(y, 5)
        y = y[pad:-pad, pad:-pad]
        return (torch.from_numpy(np.clip(y, 0.0, 1.0).astype(np.float32)).unsqueeze(0),)


POLARITY_SYSTEM = (
    "You check height maps for PBR game textures. You get one picture made of two halves: on the LEFT the colour "
    "texture, on the RIGHT its height map in grayscale, same layout. Reply with ONLY one JSON object, no prose, "
    "no code fences.")
POLARITY_PROMPT = (
    "1. In the LEFT colour texture, name the elements that physically stand out of the surface (for example stones, "
    "bricks, cobbles, rocks, planks, tiles) and the parts that are recessed between them (mortar joints, gaps, cracks, "
    "grooves).\n"
    "2. Look at the SAME places in the RIGHT grayscale image. Are the raised elements LIGHTER (whiter) than the "
    "recessed gaps between them?\n"
    'Reply exactly: {"raised": "...", "recessed": "...", "raised_are_lighter": true or false}')


class AC1_HeightPolarityCheck:
    """Builds the picture and the prompt for the VLM that checks the height polarity: colour texture on the left,
    height on the right (same size, side by side). Feed `image`, `prompt` and `system_prompt` to TextGenerate (with
    the VLM CLIP) and its reply to AC1_HeightPolarityApply."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "albedo": ("IMAGE",),
                "height": ("IMAGE",),
                "side_px": ("INT", {"default": 768, "min": 256, "max": 1536, "step": 64,
                                    "tooltip": "size of each half sent to the VLM"}),
            },
        }

    RETURN_TYPES = ("IMAGE", "STRING", "STRING")
    RETURN_NAMES = ("image", "prompt", "system_prompt")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, albedo, height, side_px):
        a = albedo[0, :, :, :3].cpu().numpy().astype(np.float32)
        hgt = to_gray(height)
        a = cv2.resize(a, (side_px, side_px), interpolation=cv2.INTER_AREA)
        hgt = cv2.resize(hgt, (side_px, side_px), interpolation=cv2.INTER_AREA)
        sep = np.ones((side_px, 8, 3), np.float32)
        pic = np.concatenate([a, sep, np.stack([hgt] * 3, -1)], axis=1)
        return (torch.from_numpy(np.clip(pic, 0, 1)).unsqueeze(0), POLARITY_PROMPT, POLARITY_SYSTEM)


class AC1_HeightPolarityApply:
    """Reads the VLM reply of the polarity check: raised_are_lighter false -> the height is inverted (white must be
    raised, as Remix displaceOut expects). An unreadable reply keeps the height as it is."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "height": ("IMAGE",),
                "llm_reply": ("STRING", {"forceInput": True}),
            },
        }

    RETURN_TYPES = ("IMAGE", "STRING", "BOOLEAN")
    RETURN_NAMES = ("height", "note", "inverted")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR/height lab"

    def run(self, height, llm_reply):
        import json
        import re
        lighter = None
        m = re.search(r"\{.*\}", llm_reply or "", re.S)
        if m:
            try:
                v = json.loads(m.group(0)).get("raised_are_lighter")
                lighter = v if isinstance(v, bool) else (str(v).strip().lower() == "true" if v is not None else None)
            except json.JSONDecodeError:
                lighter = None
        if lighter is None:
            hit = re.search(r'raised_are_lighter"?\s*:\s*(true|false)', llm_reply or "", re.I)
            lighter = (hit.group(1).lower() == "true") if hit else None
        if lighter is False:
            out, note = 1.0 - height, "VLM: raised parts were dark -> inverted"
        elif lighter is True:
            out, note = height, "VLM: raised parts are light -> kept"
        else:
            out, note = height, "VLM reply not readable -> kept"
        print(f"[AC1-PBR] height polarity: {note} | {(llm_reply or '').strip()[:300]}")
        return (out, note + "\n" + (llm_reply or ""), lighter is False)


class AC1_PackForRemix:
    """Last step after the saves: block-compresses the maps just saved for <folder>/<texture>/<variant> into the
    .ac1t files the mod uploads as they are (AC1-RTX tools/pbr_pack.py: BC1/BC3 albedo, BC5 normal / roughness /
    height, full mip chains). Without them the mod falls back to the uncompressed PNGs, which a 4096 set makes too
    big for the 32-bit game. The image inputs only order this node after the saves."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "folder": ("STRING", {"default": os.path.join(os.path.expanduser("~"), "Desktop", "AC1_PBR_prove")}),
                "texture": ("STRING", {"default": "texture"}),
                "variant": ("STRING", {"default": "x4"}),
                "pack_tool": ("STRING", {"default": r"C:\Users\manu\Desktop\MOD\AC1-RTX\tools\pbr_pack.py"}),
            },
            "optional": {
                "albedo": ("IMAGE",),
                "normal": ("IMAGE",),
                "roughness": ("IMAGE",),
                "height": ("IMAGE",),
            },
        }

    RETURN_TYPES = ("STRING",)
    RETURN_NAMES = ("report",)
    FUNCTION = "run"
    OUTPUT_NODE = True
    CATEGORY = "AC1 Remix PBR"

    def run(self, folder, texture, variant, pack_tool, albedo=None, normal=None, roughness=None, height=None):
        import importlib.util
        from pathlib import Path
        spec = importlib.util.spec_from_file_location("ac1_pbr_pack", pack_tool)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        target = Path(folder) / texture / variant if variant else Path(folder) / texture
        lines = mod.pack(target, texture)
        report = f"{texture}: " + "; ".join(lines)
        print(f"[AC1-PBR] packed for Remix: {report}")
        return {"ui": {"text": [report]}, "result": (report,)}


NODE_CLASS_MAPPINGS = {
    "AC1_DeepBumpNormals": AC1_DeepBumpNormals,
    "AC1_HeightFinalize": AC1_HeightFinalize,
    "AC1_HeightHybrid": AC1_HeightHybrid,
    "AC1_HeightLabSheet": AC1_HeightLabSheet,
    "AC1_ImageMix": AC1_ImageMix,
    "AC1_ImageBlur": AC1_ImageBlur,
    "AC1_HeightPolarityCheck": AC1_HeightPolarityCheck,
    "AC1_HeightPolarityApply": AC1_HeightPolarityApply,
    "AC1_PackForRemix": AC1_PackForRemix,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "AC1_DeepBumpNormals": "AC1 DeepBump Normals (AI, from colour)",
    "AC1_HeightFinalize": "AC1 Height Finalize (polarity + blur)",
    "AC1_HeightHybrid": "AC1 Height Hybrid (coarse + fine)",
    "AC1_HeightLabSheet": "AC1 Height Lab Sheet (compare)",
    "AC1_ImageMix": "AC1 Mix 2 immagini",
    "AC1_ImageBlur": "AC1 Sfocatura",
    "AC1_HeightPolarityCheck": "AC1 Verso height: immagine per il VLM",
    "AC1_HeightPolarityApply": "AC1 Verso height: applica la risposta del VLM",
    "AC1_PackForRemix": "AC1 Comprimi per Remix (.ac1t)",
}
