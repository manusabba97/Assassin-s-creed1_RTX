"""AC1 RTX Remix PBR helpers (experiment, everything local).

The material analysis uses the core TextGenerate node with a local VLM (Qwen3-VL). The Marigold models run on the
local GPU (downloaded once into the Hugging Face cache, or loaded from a local folder given as repo_id).

AC1_DefaultParams       base material parameters (AC1_PBR_PARAMS), no VLM review (2026-10-02)
AC1_SystemPrompt        instructions for the local VLM: what the material is, its relief, whether it has metal
AC1_PBRParams           VLM reply -> material parameters (base values; the VLM sets material, relief, metallic)
AC1_HeightPrompt        instruction for the generative image model (Qwen-Image-Edit) that draws the height map
AC1_HeightFromAI        AI height (PBRFusion4 depth) at full size, blurred by a few pixels
AC1_MarigoldMaterial    AI roughness / metallic (and albedo, unused) (prs-eth/marigold-iid-appearance-v1-1)
AC1_HeightFusion        PBRFusion4 depth + fine detail fused band by band (Laplacian pyramid)
AC1_RoughnessFusion     AI roughness + fine detail from the texture, remapped to the material's range
AC1_MetallicGate        metallic map only when the AI finds metal (else nothing is saved)
AC1_TilePad             wraps a border so every step sees the texture tiling (seamless output)
AC1_NormalFromHeight    normal map made consistent with the final height
AC1_Finalize            crops the tile border, power-of-two size
AC1_SaveToFolder        PNGs + material JSON to Desktop/AC1_PBR_prove/<texture>/
"""

import json
import os
import re

import cv2
import numpy as np
import torch

PARAMS_TYPE = "AC1_PBR_PARAMS"

DEFAULT_PARAMS = {
    "material": "unknown",
    "height_detail_weight": 0.25,     # share of the texture's fine detail in the height
    "height_contrast": 1.0,           # relief depth in the height map (>1 deeper)
    "remix_height_strength": 0.06,    # Remix relief height in metres (outward POM); x3 on 2026-10-03
    "roughness_detail_weight": 0.25,  # share of the texture's fine detail in the roughness
    "roughness_min": 0.6,
    "roughness_max": 0.9,
    "roughness_gamma": 1.0,
    "metallic": False,
    "metallic_strength": 1.0,
    "relief": "",                     # VLM: what is raised and what is recessed (drives the generated height)
}

# Keys the VLM sets (2026-10-02): the numeric map values stay at the base values above.
VLM_KEYS = ("material", "relief", "metallic")

SETTINGS = {
    "medieval": "The setting is the Holy Land during the Third Crusade (1191): every material is medieval and handmade "
                "(stone, sandstone, mud brick, lime plaster, timber, thatch, packed earth, sand, cloth, leather, rope, "
                "wrought iron, bronze, gold). Nothing is modern: if a texture looks like concrete, asphalt, plastic, "
                "sheet steel, factory tiles or paint, you are misreading it - choose the closest medieval material "
                "instead.",
    "modern (Abstergo)": "The setting is the Abstergo laboratory (2012): a modern, clean research facility "
                         "(concrete, painted plaster, polished floor tiles, glass, brushed steel, aluminium, plastic, "
                         "rubber, carpet).",
}

SYSTEM_PROMPT = """You describe a game texture (Assassin's Creed 1, 2007) for a PBR material in RTX Remix. {setting} \
Reply with ONLY one JSON object, no prose, no code fences:
{
 "material": short name of what the texture shows (e.g. "cobblestone pavement", "rubble stone wall", "lime plaster", "weathered wood planks", "rocky cliff", "packed earth with pebbles"),
 "relief": one sentence on the surface relief as seen from the front: which parts stand out and which are recessed (e.g. "rounded stones stand out, the mortar joints between them are deep grooves"),
 "metallic": true only if part of the texture is bare metal (iron, gold, bronze, steel), else false
}"""

USER_PROMPT = "Analyse this texture and return the JSON."


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def parse_params(text):
    params = dict(DEFAULT_PARAMS)
    match = re.search(r"\{.*\}", text or "", re.S)
    if not match:
        print("[AC1-PBR] no JSON in VLM reply, using defaults")
        return params, False
    try:
        reply = json.loads(match.group(0))
    except json.JSONDecodeError as e:
        print(f"[AC1-PBR] invalid JSON in VLM reply ({e}), using defaults")
        return params, False
    for key, default in DEFAULT_PARAMS.items():
        if key not in reply or key not in VLM_KEYS:
            continue
        value = reply[key]
        if isinstance(default, bool):
            params[key] = value if isinstance(value, bool) else str(value).lower() == "true"
        elif isinstance(default, float):
            try:
                params[key] = float(value)
            except (TypeError, ValueError):
                pass
        else:
            params[key] = str(value)
    params["height_detail_weight"] = _clamp(params["height_detail_weight"], 0.0, 1.0)
    params["height_contrast"] = _clamp(params["height_contrast"], 0.05, 4.0)
    params["remix_height_strength"] = _clamp(params["remix_height_strength"], 0.0, 1.0)
    params["roughness_detail_weight"] = _clamp(params["roughness_detail_weight"], 0.0, 1.0)
    params["roughness_min"] = _clamp(params["roughness_min"], 0.0, 1.0)
    params["roughness_max"] = _clamp(params["roughness_max"], params["roughness_min"], 1.0)
    params["roughness_gamma"] = _clamp(params["roughness_gamma"], 0.1, 5.0)
    params["metallic_strength"] = _clamp(params["metallic_strength"], 0.0, 1.0)
    return params, True


# ---- image helpers

def _to_gray(image):
    """IMAGE [B,H,W,C] -> float32 numpy [H,W] (first image, Rec.709 luminance)."""
    img = image[0].cpu().numpy().astype(np.float32)
    if img.shape[-1] >= 3:
        return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722
    return img[..., 0]


def _to_image(gray):
    gray = np.clip(gray, 0.0, 1.0).astype(np.float32)
    return torch.from_numpy(np.stack([gray] * 3, axis=-1)).unsqueeze(0)


def _rgb_to_image(rgb):
    return torch.from_numpy(np.clip(rgb, 0.0, 1.0).astype(np.float32)).unsqueeze(0)


def _normalize(x, lo_pct=0.5, hi_pct=99.5):
    lo, hi = np.percentile(x, [lo_pct, hi_pct])
    return np.clip((x - lo) / max(hi - lo, 1e-6), 0.0, 1.0)


def _resize_like(x, ref):
    if x.shape[:2] != ref.shape[:2]:
        x = cv2.resize(x, (ref.shape[1], ref.shape[0]), interpolation=cv2.INTER_LANCZOS4)
    return x


def _soft_contrast(h, contrast):
    """Contrast around mid-grey without clipping: a hard clip leaves flat-topped plateaus and isolated spikes in a
    height map (POM spikes). tanh curve normalised so 0 and 1 stay the extremes; contrast 1 = unchanged."""
    if abs(contrast - 1.0) < 1e-3:
        return h
    k = 2.0 * max(contrast, 0.05)
    return (0.5 + 0.5 * np.tanh(k * (h - 0.5)) / np.tanh(0.5 * k)).astype(np.float32)


def _laplacian_pyramid(x, levels):
    gauss = [x]
    for _ in range(levels):
        gauss.append(cv2.pyrDown(gauss[-1]))
    lap = []
    for i in range(levels):
        up = cv2.pyrUp(gauss[i + 1], dstsize=(gauss[i].shape[1], gauss[i].shape[0]))
        lap.append(gauss[i] - up)
    lap.append(gauss[-1])
    return lap


def _collapse(pyramid):
    x = pyramid[-1]
    for band in reversed(pyramid[:-1]):
        x = cv2.pyrUp(x, dstsize=(band.shape[1], band.shape[0])) + band
    return x


def _pyramid_fuse(base, detail, weight, levels=6):
    """Band-wise fusion: coarse bands from base only, detail's share ramps up to 2*weight (clamped) on the finest
    band, so its mean share over all bands is `weight`. Both inputs normalised to [0,1] beforehand."""
    levels = max(1, min(levels, int(np.log2(min(base.shape))) - 1))
    pb = _laplacian_pyramid(base.astype(np.float32), levels)
    pd = _laplacian_pyramid(detail.astype(np.float32), levels)
    fused = []
    for i in range(levels + 1):
        w = min(1.0, 2.0 * weight * (levels - i) / levels)
        fused.append((1.0 - w) * pb[i] + w * pd[i])
    return _collapse(fused)


# ---- Marigold (diffusers), kept in RAM between runs, on the GPU only while running

_PIPES = {}


def _marigold(kind, repo_id):
    key = (kind, repo_id)
    if key not in _PIPES:
        import diffusers
        cls = diffusers.MarigoldIntrinsicsPipeline
        dtype = torch.float16 if torch.cuda.is_available() else torch.float32
        try:
            pipe = cls.from_pretrained(repo_id, variant="fp16", torch_dtype=dtype)
        except (OSError, ValueError):
            pipe = cls.from_pretrained(repo_id, torch_dtype=dtype)
        pipe.set_progress_bar_config(disable=True)
        _PIPES[key] = pipe
    return _PIPES[key]


def _run_marigold(kind, repo_id, image, steps, ensemble_size, processing_resolution):
    import comfy.model_management as mm
    pipe = _marigold(kind, repo_id)
    mm.unload_all_models()  # free the VRAM held by the upscaler / VLM
    device = mm.get_torch_device()
    pipe.to(device)
    try:
        img = image[0, :, :, :3].permute(2, 0, 1).unsqueeze(0).to(device, pipe.dtype)
        gen = torch.Generator(device=device).manual_seed(0)
        out = pipe(img, num_inference_steps=steps, ensemble_size=ensemble_size,
                   processing_resolution=processing_resolution, generator=gen, output_type="pt")
        return pipe, out.prediction.float().cpu()
    finally:
        pipe.to("cpu")
        mm.soft_empty_cache()


_MARIGOLD_INPUTS = {
    "steps": ("INT", {"default": 4, "min": 1, "max": 50}),
    "ensemble_size": ("INT", {"default": 3, "min": 1, "max": 10,
                              "tooltip": "independent predictions averaged; higher = steadier, slower"}),
    "processing_resolution": ("INT", {"default": 1024, "min": 0, "max": 2048, "step": 64,
                                      "tooltip": "0 = input resolution"}),
}


# ---- nodes

class AC1_SystemPrompt:
    """Fixed instructions for the local VLM, with the historical setting of the texture (Masyaf and the other
    cities are medieval, the Abstergo lab is modern); AC1_PBRParams parses its reply."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"setting": (list(SETTINGS), {"default": "medieval"})}}

    RETURN_TYPES = ("STRING", "STRING")
    RETURN_NAMES = ("prompt", "system_prompt")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, setting):
        return (USER_PROMPT, SYSTEM_PROMPT.replace("{setting}", SETTINGS[setting]))


class AC1_PBRParams:
    """Parses the VLM reply into material parameters (defaults when it is not valid JSON)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"llm_reply": ("STRING", {"forceInput": True})}}

    RETURN_TYPES = (PARAMS_TYPE, "STRING", "BOOLEAN", "FLOAT")
    RETURN_NAMES = ("params", "summary", "metallic", "remix_height_strength")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, llm_reply):
        params, ok = parse_params(llm_reply)
        summary = ("" if ok else "VLM reply not parsed, defaults used\n") + json.dumps(params, indent=1)
        print(f"[AC1-PBR] material params: {json.dumps(params)}")
        return (params, summary, params["metallic"], params["remix_height_strength"])


class AC1_HeightPrompt:
    """Instruction for Qwen-Image-Edit: redraw the input texture as its grayscale height map, same framing, using the
    VLM's material and relief description (generic wording when the VLM gave none)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"params": (PARAMS_TYPE,)},
                "optional": {"source": (["albedo", "normal map"], {"default": "albedo",
                                         "tooltip": "what the edited image is: the colour texture or its normal map"})}}

    RETURN_TYPES = ("STRING",)
    RETURN_NAMES = ("prompt",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, params, source="albedo"):
        material = params.get("material") or "surface"
        relief = params.get("relief") or "the raised parts stand out and the gaps between them are recessed"
        if source == "normal map":
            prompt = (f"This image is the tangent-space normal map of a {material} texture (red = slope left/right, "
                      "green = slope up/down, flat areas are lavender blue). Convert it into the matching grayscale "
                      "height map (displacement map), integrating the slopes. "
                      f"{(relief[0].upper() + relief[1:]).rstrip(".") + "." if relief else ''} "
                      "White is the highest raised surface, black is the deepest recess, mid grey in between. "
                      "Keep exactly the same layout, shapes, positions and scale, pixel aligned, seamless. "
                      "Pure grayscale, no colour, no lighting, no shading, no text.")
            print(f"[AC1-PBR] height prompt (normal map): {prompt}")
            return (prompt,)
        prompt = (f"Convert this {material} texture into its grayscale height map (displacement map) for 3D rendering. "
                  f"{(relief[0].upper() + relief[1:]).rstrip(".") + "." if relief else ''} "
                  "White is the highest raised surface, black is the deepest recess, mid grey in between. "
                  "Keep exactly the same layout, shapes, positions and scale as the input image, pixel aligned, "
                  "seamless. Pure grayscale, no colour, no lighting, no shadows, no shading, no text.")
        print(f"[AC1-PBR] height prompt: {prompt}")
        return (prompt,)


class AC1_DefaultParams:
    """The base material parameters (DEFAULT_PARAMS), without the VLM review: the user found the base values good."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {}}

    RETURN_TYPES = (PARAMS_TYPE,)
    RETURN_NAMES = ("params",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self):
        return (dict(DEFAULT_PARAMS),)


class AC1_HeightFromAI:
    """Height from the AI (PBRFusion4 depth, inferred from the image): brought to the reference's size (Lanczos),
    normalised (0.5-99.5 percentile), then blurred by blur_px pixels (Gaussian, 2 sigma = blur_px) so POM steps
    stay smooth; contrast is the soft curve around mid-grey (1 = unchanged). invert flips the polarity."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "depth": ("IMAGE",),
                "reference": ("IMAGE",),
                "blur_px": ("FLOAT", {"default": 4.0, "min": 0.0, "max": 64.0, "step": 0.5}),
                "contrast": ("FLOAT", {"default": 1.0, "min": 0.05, "max": 4.0, "step": 0.05}),
                "invert": ("BOOLEAN", {"default": False}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("height",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, depth, reference, blur_px, contrast, invert):
        H, W = reference.shape[1:3]
        h = _to_gray(depth)
        if h.shape != (H, W):
            h = cv2.resize(h, (W, H), interpolation=cv2.INTER_LANCZOS4)
        h = _normalize(h)
        if invert:
            h = 1.0 - h
        if blur_px >= 0.5:
            r = int(np.ceil(blur_px))
            h = cv2.GaussianBlur(h, (2 * r + 1, 2 * r + 1), blur_px / 2.0, borderType=cv2.BORDER_REFLECT)
            h = _normalize(h, 0.0, 100.0)
        h = _soft_contrast(h, contrast)
        return (_to_image(h),)


class AC1_MarigoldMaterial:
    """Marigold IID Appearance: albedo without baked lighting, roughness and metallicity, pixel-aligned. Sub-targets
    are looked up by name in the model's target_properties."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                **_MARIGOLD_INPUTS,
                "repo_id": ("STRING", {"default": "prs-eth/marigold-iid-appearance-v1-1",
                                       "tooltip": "Hugging Face id (downloaded once) or a local folder"}),
            },
            "optional": {
                "params": (PARAMS_TYPE, {"tooltip": "metallic_strength scales the metallic output"}),
            },
        }

    RETURN_TYPES = ("IMAGE", "IMAGE", "IMAGE")
    RETURN_NAMES = ("albedo", "roughness", "metallic")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, image, steps, ensemble_size, processing_resolution, repo_id, params=None):
        pipe, pred = _run_marigold("intrinsics", repo_id, image, steps, ensemble_size, processing_resolution)
        props = pipe.target_properties
        found = {}
        for t_idx, name in enumerate(props["target_names"]):
            entry = props.get(name, {})
            if entry.get("prediction_space") == "stack":
                for c, sub in enumerate(entry.get("sub_target_names", [])):
                    if sub:
                        found[sub] = pred[t_idx, c].numpy()
            else:
                found[name] = pred[t_idx].permute(1, 2, 0).numpy()
        missing = {"albedo", "roughness", "metallicity"} - set(found)
        if missing:
            raise RuntimeError(f"[AC1-PBR] model {repo_id} lacks {sorted(missing)}: has {sorted(found)}")
        albedo = found["albedo"]
        if albedo.ndim == 2:
            albedo = np.stack([albedo] * 3, axis=-1)
        strength = params["metallic_strength"] if params else 1.0
        return (_rgb_to_image(albedo), _to_image(found["roughness"]), _to_image(found["metallicity"] * strength))


class AC1_HeightFusion:
    """Fuses a smooth depth map (shape) with a detail map (micro relief) band by band in a Laplacian pyramid: coarse
    bands from depth only, the detail's share grows toward the finest bands and averages detail_weight. Then
    optional smoothing and contrast around mid-grey."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "depth": ("IMAGE",),
                "detail": ("IMAGE",),
                "detail_weight": ("FLOAT", {"default": 0.25, "min": 0.0, "max": 1.0, "step": 0.01}),
                "height_contrast": ("FLOAT", {"default": 1.0, "min": 0.05, "max": 4.0, "step": 0.05}),
                "blur_percent": ("FLOAT", {"default": 0.0, "min": 0.0, "max": 20.0, "step": 0.1,
                                           "tooltip": "smoothing after fusion: blur radius as % of the short side"}),
            },
            "optional": {
                "params": (PARAMS_TYPE, {"tooltip": "overrides detail_weight and height_contrast"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("height",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, depth, detail, detail_weight, height_contrast, blur_percent, params=None):
        if params:
            detail_weight = params["height_detail_weight"]
            height_contrast = params["height_contrast"]
        d = _normalize(_to_gray(depth))
        t = _normalize(_resize_like(_to_gray(detail), d))
        h = _pyramid_fuse(d, t, detail_weight)
        radius = blur_percent / 100.0 * min(h.shape)
        if radius >= 0.5:
            h = cv2.GaussianBlur(h, (0, 0), sigmaX=radius / 3.0)
        h = _normalize(h)
        h = _soft_contrast(h, height_contrast)
        return (_to_image(h),)


class AC1_RoughnessFusion:
    """AI roughness as the base (separates materials, ignores colour), the texture's fine pattern fused in band by
    band. The detail's sign follows the measured correlation between the AI roughness and the texture's luminance,
    so no invert guess. Then gamma, contrast, and a percentile remap to [roughness_min, roughness_max]."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "ai_roughness": ("IMAGE",),
                "detail_weight": ("FLOAT", {"default": 0.25, "min": 0.0, "max": 1.0, "step": 0.01}),
                "roughness_min": ("FLOAT", {"default": 0.6, "min": 0.0, "max": 1.0, "step": 0.01}),
                "roughness_max": ("FLOAT", {"default": 0.9, "min": 0.0, "max": 1.0, "step": 0.01}),
                "gamma": ("FLOAT", {"default": 1.0, "min": 0.1, "max": 5.0, "step": 0.05}),
                "contrast": ("FLOAT", {"default": 1.0, "min": 0.1, "max": 4.0, "step": 0.01}),
            },
            "optional": {
                "detail": ("IMAGE", {"tooltip": "fine pattern source, e.g. the upscaled texture"}),
                "params": (PARAMS_TYPE, {"tooltip": "overrides detail_weight, min, max, gamma"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("roughness",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, ai_roughness, detail_weight, roughness_min, roughness_max, gamma, contrast, detail=None,
            params=None):
        if params:
            detail_weight = params["roughness_detail_weight"]
            roughness_min = params["roughness_min"]
            roughness_max = params["roughness_max"]
            gamma = params["roughness_gamma"]
        r = _normalize(_to_gray(ai_roughness), 1.0, 99.0)
        if detail is not None and detail_weight > 0:
            t = _normalize(_resize_like(_to_gray(detail), r), 1.0, 99.0)
            corr = np.corrcoef(r.ravel(), t.ravel())[0, 1]
            if np.isfinite(corr) and corr < 0:
                t = 1.0 - t
            r = _normalize(_pyramid_fuse(r, t, detail_weight), 1.0, 99.0)
        r = np.power(r, gamma)
        r = np.clip(0.5 + (r - 0.5) * contrast, 0.0, 1.0)
        r = roughness_min + r * (roughness_max - roughness_min)
        return (_to_image(r),)


class AC1_MetallicGate:
    """Keeps the AI metallic map only when there is metal: the VLM (params) must see bare metal, and the AI map must
    have at least coverage_percent of its pixels at or above threshold (Marigold alone gives ~0.5 on grey stone).
    Otherwise outputs black and has_metal = False, and AC1_SaveToFolder (skip_if_empty) saves nothing."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "metallic": ("IMAGE",),
                "threshold": ("FLOAT", {"default": 0.8, "min": 0.0, "max": 1.0, "step": 0.01}),
                "coverage_percent": ("FLOAT", {"default": 0.5, "min": 0.0, "max": 100.0, "step": 0.1}),
            },
            "optional": {
                "params": (PARAMS_TYPE, {"tooltip": "unused (kept so older workflows load)"}),
                "reference": ("IMAGE", {"tooltip": "unused (kept so older workflows load)"}),
            },
        }

    RETURN_TYPES = ("IMAGE", "BOOLEAN")
    RETURN_NAMES = ("metallic", "has_metal")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, metallic, threshold, coverage_percent, params=None, reference=None):
        if params is not None and not params.get("metallic", False):
            print("[AC1-PBR] metallic: the VLM sees no bare metal, no metallic map")
            return (torch.zeros_like(metallic[:1, :, :, :3]), False)
        m = _to_gray(metallic)
        coverage = float(np.mean(m >= threshold)) * 100.0
        has_metal = coverage >= coverage_percent and coverage > 0.0
        print(f"[AC1-PBR] metallic: {coverage:.2f}% of pixels >= {threshold:.2f} -> "
              f"{'metal' if has_metal else 'no metal, no metallic map'}")
        if has_metal:
            return (metallic, True)
        return (torch.zeros_like(metallic[:1, :, :, :3]), False)


PAD_TYPE = "AC1_TILE_PAD"


class AC1_TilePad:
    """Seamless tiling: wraps a border copied from the opposite side around the texture (and its mask), so every
    later step (upscaler, PBRFusion, Marigold, filters) sees the texture as it tiles in game. AC1_Finalize crops the
    border away at the end; the pad is stored as fractions, so it survives any upscale factor."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "seamless": ("BOOLEAN", {"default": True, "tooltip": "off = no border, image passes unchanged"}),
                "pad_percent": ("FLOAT", {"default": 12.5, "min": 0.0, "max": 50.0, "step": 0.5,
                                          "tooltip": "border on each side, % of the side length"}),
            },
            "optional": {
                "mask": ("MASK",),
            },
        }

    RETURN_TYPES = ("IMAGE", "MASK", PAD_TYPE)
    RETURN_NAMES = ("image", "mask", "pad")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, image, seamless, pad_percent, mask=None):
        _, h, w, _ = image.shape
        if mask is None or tuple(mask.shape[-2:]) != (h, w):
            mask = torch.zeros((1, h, w), dtype=image.dtype)
        if not seamless or pad_percent <= 0:
            return (image, mask, {"x": 0.0, "y": 0.0})
        px = int(round(w * pad_percent / 100.0))
        py = int(round(h * pad_percent / 100.0))
        img = torch.nn.functional.pad(image.permute(0, 3, 1, 2), (px, px, py, py), mode="circular").permute(0, 2, 3, 1)
        m = torch.nn.functional.pad(mask.unsqueeze(1), (px, px, py, py), mode="circular").squeeze(1)
        return (img, m, {"x": px / (w + 2 * px), "y": py / (h + 2 * py)})


class AC1_Finalize:
    """Last step before saving: crops the AC1_TilePad border, scales (0.5 = the x2 version from the x4 output),
    then resizes to the nearest power of two per side (capped at max_size), as DDS / Remix textures want.
    normal: re-normalised after resizing (OpenGL encoding)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "map_type": (["color", "data", "normal"], {"default": "data"}),
                "scale": ("FLOAT", {"default": 1.0, "min": 0.05, "max": 1.0, "step": 0.05,
                                    "tooltip": "downscale after cropping (0.5 = half resolution)"}),
                "power_of_two": ("BOOLEAN", {"default": True}),
                "max_size": ("INT", {"default": 4096, "min": 64, "max": 16384, "step": 64}),
            },
            "optional": {
                "pad": (PAD_TYPE,),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, image, map_type, scale, power_of_two, max_size, pad=None):
        img = image[0].cpu().numpy().astype(np.float32)
        h, w = img.shape[:2]
        if pad:
            px = int(round(pad["x"] * w))
            py = int(round(pad["y"] * h))
            img = img[py:h - py, px:w - px]
            h, w = img.shape[:2]
        sw, sh = max(1.0, w * scale), max(1.0, h * scale)
        if power_of_two:
            tw = min(max_size, 2 ** int(round(np.log2(sw))))
            th = min(max_size, 2 ** int(round(np.log2(sh))))
        else:
            cap = min(1.0, max_size / max(sw, sh))
            tw, th = int(round(sw * cap)), int(round(sh * cap))
        if (tw, th) != (w, h):
            interp = cv2.INTER_AREA if tw * th < w * h else cv2.INTER_LANCZOS4
            img = cv2.resize(img, (tw, th), interpolation=interp)
            if img.ndim == 2:
                img = img[..., None]
        if map_type == "normal":
            n = img[..., :3] * 2.0 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            img = np.concatenate([n * 0.5 + 0.5, img[..., 3:]], axis=-1)
        return (torch.from_numpy(np.clip(img, 0.0, 1.0).astype(np.float32)).unsqueeze(0),)


class AC1_NormalFromHeight:
    """Makes the normal map agree with the final height (the one Remix uses for parallax). The height's normal is
    computed with Sobel and its slope is scaled to match the mean slope of the input normal (measured, not guessed),
    then blended with the input: blend 0 = input normal only, 1 = normal from the height only. OpenGL encoding
    (green = up), the convention measured on PBRFusion4's output."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "normal": ("IMAGE",),
                "height": ("IMAGE",),
                "blend": ("FLOAT", {"default": 0.5, "min": 0.0, "max": 1.0, "step": 0.05}),
                "strength": ("FLOAT", {"default": 1.0, "min": 0.1, "max": 5.0, "step": 0.05,
                                       "tooltip": "multiplies the matched slope of the height's normal"}),
                "seamless": ("BOOLEAN", {"default": True, "tooltip": "wrap at the borders (tiling textures)"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("normal",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, normal, height, blend, strength, seamless):
        n_in = normal[0, :, :, :3].cpu().numpy().astype(np.float32) * 2.0 - 1.0
        n_in /= np.maximum(np.linalg.norm(n_in, axis=-1, keepdims=True), 1e-6)
        h = _resize_like(_to_gray(height), n_in)
        # OpenCV filters reject BORDER_WRAP: extend by one pixel by hand, filter, crop
        hp = np.pad(h, 1, mode="wrap" if seamless else "reflect")
        dx = cv2.Sobel(hp, cv2.CV_32F, 1, 0, ksize=3)[1:-1, 1:-1] / 8.0
        drow = cv2.Sobel(hp, cv2.CV_32F, 0, 1, ksize=3)[1:-1, 1:-1] / 8.0
        # match the height normal's mean slope to the input's so the blend flattens neither
        slope_in = np.mean(np.hypot(n_in[..., 0], n_in[..., 1]) / np.maximum(n_in[..., 2], 0.05))
        slope_h = np.mean(np.hypot(dx, drow))
        k = strength * slope_in / max(slope_h, 1e-8)
        n_h = np.stack([-dx * k, drow * k, np.ones_like(h)], axis=-1)  # OpenGL: y up, rows go down
        n_h /= np.linalg.norm(n_h, axis=-1, keepdims=True)
        n = (1.0 - blend) * n_in + blend * n_h
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
        return (_rgb_to_image(n * 0.5 + 0.5),)


class AC1_SaveToFolder:
    """Saves PNGs outside ComfyUI's output folder: <folder>/<texture>/<variant>/<texture>_<map>_00001.png.
    RGBA inputs keep their alpha (dropped when fully opaque). With params connected it also writes
    <texture>_material_00001.json for the Remix material (maps, normal encoding, suggested heightTextureStrength,
    VLM parameters)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "images": ("IMAGE",),
                "folder": ("STRING", {"default": os.path.join(os.path.expanduser("~"), "Desktop", "AC1_PBR_prove")}),
                "texture": ("STRING", {"default": "texture"}),
                "map": ("STRING", {"default": "albedo"}),
                "variant": ("STRING", {"default": "x4", "tooltip": "sub-folder, e.g. x4 / x2"}),
            },
            "optional": {
                "params": (PARAMS_TYPE,),
                "skip_if_empty": ("BOOLEAN", {"default": False,
                                              "tooltip": "save nothing when the map is all black (no metal)"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("images",)
    FUNCTION = "run"
    OUTPUT_NODE = True

    _run_index = {}  # (out_dir, texture, prompt id) -> NNNNN shared by every map saved in that run

    @staticmethod
    def _index(out_dir, texture):
        """Every map of one run gets the same NNNNN (1 + the highest of any map of the texture), so a map that a run
        does not save (metallic without metal) is recognisably absent from that run."""
        try:
            import server
            prompt_id = getattr(server.PromptServer.instance, "last_prompt_id", None)
        except Exception:
            prompt_id = None
        key = (out_dir, texture, prompt_id)
        if prompt_id is None or key not in AC1_SaveToFolder._run_index:
            top = 0
            for f in os.listdir(out_dir):
                m = re.fullmatch(rf"{re.escape(texture)}_[a-z]+_(\d+)\.(png|json)", f)
                if m:
                    top = max(top, int(m.group(1)))
            AC1_SaveToFolder._run_index[key] = top + 1
        return AC1_SaveToFolder._run_index[key]
    CATEGORY = "AC1 Remix PBR"

    def run(self, images, folder, texture, map, variant, params=None, skip_if_empty=False):
        from PIL import Image
        import nodes
        out_dir = os.path.join(folder, texture, variant) if variant else os.path.join(folder, texture)
        os.makedirs(out_dir, exist_ok=True)
        base = f"{texture}_{map}"
        index = self._index(out_dir, texture)
        if skip_if_empty and float(images.max()) <= 0.0:
            print(f"[AC1-PBR] {base}: empty map, not saved (run {index:05})")
            return {"ui": {}, "result": (images,)}
        for b in range(images.shape[0]):
            arr = np.clip(images[b].cpu().numpy() * 255.0, 0, 255).astype(np.uint8)
            if arr.shape[-1] == 4 and arr[..., 3].min() == 255:
                arr = arr[..., :3]
            Image.fromarray(arr).save(os.path.join(out_dir, f"{base}_{index + b:05}.png"), compress_level=4)
        if params:
            material = {
                "texture": texture,
                "variant": variant,
                "run": index,
                "maps": {m: f"{texture}_{m}_{index:05}.png"
                         for m in ("albedo", "normal", "height", "roughness", "metallic")},
                "normal_encoding": "OpenGL (green up)",
                "remix_height_strength": params["remix_height_strength"],
                "params": params,
            }
            with open(os.path.join(out_dir, f"{texture}_material_{index:05}.json"), "w", encoding="utf-8") as f:
                json.dump(material, f, indent=1)
        print(f"[AC1-PBR] saved {base}_{index:05} to {out_dir}")
        preview = nodes.PreviewImage().save_images(images[..., :3], filename_prefix=f"ac1_{map}")
        return {"ui": preview["ui"], "result": (images,)}


def _frankot_chellappa(p, q, seamless):
    """Height z from gradients p = dz/dx (columns), q = dz/drow (rows), least squares in the Fourier domain.
    seamless: the texture tiles, solve periodically; otherwise mirror-extend so the borders do not wrap."""
    h, w = p.shape
    if not seamless:
        p = np.block([[p, -p[:, ::-1]], [p[::-1, :], -p[::-1, ::-1]]])
        q = np.block([[q, q[:, ::-1]], [-q[::-1, :], -q[::-1, ::-1]]])
    H, W = p.shape
    wx = np.fft.fftfreq(W) * 2.0 * np.pi
    wy = np.fft.fftfreq(H) * 2.0 * np.pi
    WX, WY = np.meshgrid(wx, wy)
    denom = WX ** 2 + WY ** 2
    denom[0, 0] = 1.0
    Z = (-1j * WX * np.fft.fft2(p) - 1j * WY * np.fft.fft2(q)) / denom
    Z[0, 0] = 0.0
    return np.real(np.fft.ifft2(Z))[:h, :w].astype(np.float32)


def _np_image(image):
    return image[0].cpu().numpy().astype(np.float32)


def _gray_np(img):
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


class AC1_Deblock:
    """Removes DXT (4x4 block) compression artefacts before the upscaler, which otherwise reads the block grid as a
    texture (woven patterns): edge-preserving bilateral filter on RGB, alpha untouched. strength 0 = off."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "image": ("IMAGE",),
                "strength": ("FLOAT", {"default": 0.5, "min": 0.0, "max": 1.0, "step": 0.05}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, image, strength):
        if strength <= 0:
            return (image,)
        img = _np_image(image)
        rgb = np.ascontiguousarray(img[..., :3])
        filtered = cv2.bilateralFilter(rgb, d=5, sigmaColor=0.02 + 0.10 * strength, sigmaSpace=1.5 + 1.5 * strength)
        out = img.copy()
        out[..., :3] = rgb + (filtered - rgb) * strength
        return (torch.from_numpy(np.clip(out, 0.0, 1.0)).unsqueeze(0),)


class AC1_UpscaleGuard:
    """Keeps the AI upscale faithful to the original texture.
    1. fidelity = correlation between the upscale brought back to the original size and the original (luminance).
    2. structure lock: colours and shapes (frequencies below lock_px original pixels) come from the original
       (Lanczos-enlarged); only the finer detail comes from the AI upscale.
    3. below fidelity_threshold the AI detail is discarded (the AI invented content): Lanczos + unsharp mask.
    Alpha comes from the upscale when it has one."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "upscaled": ("IMAGE",),
                "original": ("IMAGE",),
                "lock_px": ("FLOAT", {"default": 1.0, "min": 0.25, "max": 8.0, "step": 0.25,
                                      "tooltip": "structure taken from the original down to this size (original px)"}),
                "fidelity_threshold": ("FLOAT", {"default": 0.9, "min": 0.0, "max": 1.0, "step": 0.01}),
                "fallback_sharpen": ("FLOAT", {"default": 0.6, "min": 0.0, "max": 3.0, "step": 0.05}),
            },
        }

    RETURN_TYPES = ("IMAGE", "STRING", "FLOAT")
    RETURN_NAMES = ("image", "report", "fidelity")
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, upscaled, original, lock_px, fidelity_threshold, fallback_sharpen):
        up = _np_image(upscaled)
        orig = _np_image(original)
        H, W = up.shape[:2]
        h, w = orig.shape[:2]
        scale = W / w
        down = cv2.resize(up[..., :3], (w, h), interpolation=cv2.INTER_AREA)
        fidelity = float(np.corrcoef(_gray_np(down).ravel(), _gray_np(orig[..., :3]).ravel())[0, 1])
        ref = cv2.resize(orig[..., :3], (W, H), interpolation=cv2.INTER_LANCZOS4)
        sigma = max(0.5, lock_px * scale)
        if fidelity >= fidelity_threshold:
            rgb = cv2.GaussianBlur(ref, (0, 0), sigma) + (up[..., :3] - cv2.GaussianBlur(up[..., :3], (0, 0), sigma))
            mode = "AI detail on locked structure"
        else:
            rgb = ref + fallback_sharpen * (ref - cv2.GaussianBlur(ref, (0, 0), max(0.5, 0.5 * scale)))
            mode = "AI discarded (invented content): Lanczos + sharpen"
        out = np.clip(rgb, 0.0, 1.0)
        if up.shape[-1] == 4:
            out = np.concatenate([out, up[..., 3:4]], axis=-1)
        report = f"fidelity {fidelity:.3f} (threshold {fidelity_threshold:.2f}): {mode}"
        print(f"[AC1-PBR] upscale guard: {report}")
        return (torch.from_numpy(out.astype(np.float32)).unsqueeze(0), report, fidelity)


class AC1_MatchNormal:
    """Resizes a tangent-space normal map to the reference image's size (Lanczos) and re-normalises it. strength
    scales the X/Y slopes before re-normalising (1 = unchanged, 1.2 = 20% more intense relief)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"normal": ("IMAGE",), "reference": ("IMAGE",)},
                "optional": {"strength": ("FLOAT", {"default": 1.0, "min": 0.0, "max": 4.0, "step": 0.05})}}

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("normal",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, normal, reference, strength=1.0):
        n = _np_image(normal)[..., :3]
        H, W = reference.shape[1:3]
        if n.shape[:2] != (H, W):
            n = cv2.resize(n, (W, H), interpolation=cv2.INTER_LANCZOS4)
        v = n * 2.0 - 1.0
        v[..., :2] *= strength
        v[..., 2] = np.maximum(v[..., 2], 0.0)
        v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
        return (_rgb_to_image(v * 0.5 + 0.5),)


class AC1_HeightFromNormals:
    """Height integrated from normal maps (Frankot-Chellappa), OpenGL convention (green = up, the engine's).
    normal: the game's own normal map (the authored relief, enlarged) gives the shapes; detail_normal (e.g. the
    PBRFusion normal computed on the upscaled albedo, the one used in game) is integrated too and fused band by band
    into the finest bands (detail_weight), so the height has full-resolution relief. Undulations larger than
    flatten_percent of the short side are removed; contrast is a soft curve (no clipping, which made POM spikes)."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "normal": ("IMAGE",),
                "opengl": ("BOOLEAN", {"default": True}),
                "seamless": ("BOOLEAN", {"default": False, "tooltip": "true only for an unpadded tiling texture"}),
                "detail_weight": ("FLOAT", {"default": 0.25, "min": 0.0, "max": 1.0, "step": 0.01}),
                "height_contrast": ("FLOAT", {"default": 1.0, "min": 0.05, "max": 4.0, "step": 0.05}),
                "blur_percent": ("FLOAT", {"default": 0.0, "min": 0.0, "max": 20.0, "step": 0.1}),
                "flatten_percent": ("FLOAT", {"default": 3.0, "min": 0.0, "max": 100.0, "step": 0.5,
                                              "tooltip": "removes undulations larger than this % of the short side"}),
            },
            "optional": {
                "detail_normal": ("IMAGE", {"tooltip": "full-resolution normal map whose relief fills the fine bands"}),
                "params": (PARAMS_TYPE, {"tooltip": "overrides detail_weight and height_contrast"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("height",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    @staticmethod
    def _integrate(normal, opengl, seamless, flatten_percent, size=None):
        n = _np_image(normal)[..., :3]
        if size is not None and n.shape[:2] != size:
            n = cv2.resize(n, (size[1], size[0]), interpolation=cv2.INTER_LANCZOS4)
        n = n * 2.0 - 1.0
        if not opengl:
            n[..., 1] = -n[..., 1]
        nz = np.maximum(n[..., 2], 0.2)
        h = _frankot_chellappa(-n[..., 0] / nz, n[..., 1] / nz, seamless)
        if flatten_percent > 0:
            h = h - cv2.GaussianBlur(h, (0, 0), flatten_percent / 100.0 * min(h.shape), borderType=cv2.BORDER_REFLECT)
        return _normalize(h)

    def run(self, normal, opengl, seamless, detail_weight, height_contrast, blur_percent, flatten_percent,
            detail_normal=None, params=None):
        if params:
            detail_weight = params["height_detail_weight"]
            height_contrast = params["height_contrast"]
        h = self._integrate(normal, opengl, seamless, flatten_percent)
        if detail_normal is not None and detail_weight > 0:
            d = self._integrate(detail_normal, opengl, seamless, flatten_percent, h.shape)
            h = _normalize(_pyramid_fuse(h, d, detail_weight))
        radius = blur_percent / 100.0 * min(h.shape)
        if radius >= 0.5:
            h = _normalize(cv2.GaussianBlur(h, (0, 0), radius / 3.0))
        h = _soft_contrast(h, height_contrast)
        return (_to_image(h),)


class AC1_CapSize:
    """Downscales (area filter) so the longer side is at most max_side; smaller images pass unchanged. Used before
    PBRFusion4, whose full-resolution pass runs out of VRAM above ~2560 px (5120 px requested 12.5 GiB on a 16 GB
    card); its normal is brought back to full size with AC1_MatchNormal."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"image": ("IMAGE",), "max_side": ("INT", {"default": 2560, "min": 256, "max": 16384})}}

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    CATEGORY = "AC1 Remix PBR"

    def run(self, image, max_side):
        h, w = image.shape[1:3]
        if max(h, w) <= max_side:
            return (image,)
        k = max_side / max(h, w)
        img = cv2.resize(_np_image(image), (round(w * k), round(h * k)), interpolation=cv2.INTER_AREA)
        if img.ndim == 2:
            img = img[..., None]
        return (torch.from_numpy(img).unsqueeze(0),)


NODE_CLASS_MAPPINGS = {
    "AC1_HeightPrompt": AC1_HeightPrompt,
    "AC1_DefaultParams": AC1_DefaultParams,
    "AC1_HeightFromAI": AC1_HeightFromAI,
    "AC1_SystemPrompt": AC1_SystemPrompt,
    "AC1_PBRParams": AC1_PBRParams,
    "AC1_MarigoldMaterial": AC1_MarigoldMaterial,
    "AC1_HeightFusion": AC1_HeightFusion,
    "AC1_RoughnessFusion": AC1_RoughnessFusion,
    "AC1_MetallicGate": AC1_MetallicGate,
    "AC1_SaveToFolder": AC1_SaveToFolder,
    "AC1_TilePad": AC1_TilePad,
    "AC1_Finalize": AC1_Finalize,
    "AC1_NormalFromHeight": AC1_NormalFromHeight,
    "AC1_Deblock": AC1_Deblock,
    "AC1_UpscaleGuard": AC1_UpscaleGuard,
    "AC1_MatchNormal": AC1_MatchNormal,
    "AC1_HeightFromNormals": AC1_HeightFromNormals,
    "AC1_CapSize": AC1_CapSize,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "AC1_HeightPrompt": "AC1 Height Prompt (for Qwen-Image-Edit)",
    "AC1_DefaultParams": "AC1 Default Params (no VLM)",
    "AC1_HeightFromAI": "AC1 Height from AI (blurred)",
    "AC1_SystemPrompt": "AC1 Material Prompt (VLM)",
    "AC1_PBRParams": "AC1 PBR Params (from VLM)",
    "AC1_MarigoldMaterial": "AC1 Marigold Albedo/Roughness/Metallic (AI)",
    "AC1_HeightFusion": "AC1 Height Fusion (pyramid)",
    "AC1_RoughnessFusion": "AC1 Roughness (AI + detail)",
    "AC1_MetallicGate": "AC1 Metallic Gate",
    "AC1_SaveToFolder": "AC1 Save to Folder",
    "AC1_TilePad": "AC1 Tile Pad (seamless)",
    "AC1_Finalize": "AC1 Finalize (crop + power of two)",
    "AC1_NormalFromHeight": "AC1 Normal from Height (blend)",
    "AC1_Deblock": "AC1 Deblock (DXT)",
    "AC1_UpscaleGuard": "AC1 Upscale Guard (fidelity)",
    "AC1_MatchNormal": "AC1 Match Normal",
    "AC1_HeightFromNormals": "AC1 Height from Normals",
    "AC1_CapSize": "AC1 Cap Size",
}


# Height lab (comparison of height-map methods), registered with the other AC1 nodes.
from .height_lab import NODE_CLASS_MAPPINGS as _LAB_NODES, NODE_DISPLAY_NAME_MAPPINGS as _LAB_NAMES  # noqa: E402

NODE_CLASS_MAPPINGS.update(_LAB_NODES)
NODE_DISPLAY_NAME_MAPPINGS.update(_LAB_NAMES)
