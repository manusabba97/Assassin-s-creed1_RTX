"""Builds the height-lab ComfyUI workflow (10 height-map proposals from one texture) in both forms from one graph:

  tools/height_lab_api.json                          API prompt (tools/height_lab.py runs it in batch)
  ComfyUI/user/default/workflows/AC1_height_lab.json  UI workflow for the ComfyUI frontend

Node schemas (input order, widget kinds) come from a running ComfyUI's /object_info, saved to a JSON file:
  python tools/height_lab_build.py <object_info.json>

The upscaler front (deblock, tile pad, SeedVR2 x4) and the VLM are the ones of concept_flussoPBR_AC1remix.json.
"""

import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
COMFY_USER = Path(r"C:\Users\manu\AppData\Local\Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI\user\default")
MAIN_UI = COMFY_USER / "workflows" / "concept_flussoPBR_AC1remix.json"
MAIN_API = TOOLS / "pbr_workflow_api.json"
OUT_API = TOOLS / "height_lab_api.json"
OUT_UI = COMFY_USER / "workflows" / "AC1_height_lab.json"

R = lambda node, slot=0: [str(node), slot]  # noqa: E731  link to output `slot` of `node`

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

# ---- graph: id -> (class, inputs, title, column, row). Links are R(node, slot); everything else is a widget value.
G = {}


def n(nid, cls, title, col, row, **inputs):
    G[str(nid)] = (cls, inputs, title, col, row)


# shared front: albedo, game normal, VLM (upscaler 66:* is copied from the main workflow)
n(1, "LoadImage", "Albedo (texture)", 0, 0, image="ac1pbr_326AE55313C5573F.png")
n(300, "LoadImage", "Normal di gioco", 0, 3, image="ac1pbr_326AE55313C5573F_normal.png")
n(231, "PrimitiveString", "Nome texture", 0, 6, value="326AE55313C5573F")
n(301, "AC1_Deblock", "Deblock", 1, 0, image=R(1), strength=0.5)
n(230, "AC1_TilePad", "Tile pad", 1, 1, image=R(301), seamless=True, pad_percent=12.5, mask=R(1, 1))
n(131, "RGBAtoRGB", "Albedo x4 (upscaled)", 3, 0, image=R("66:59"))
n(302, "AC1_MatchNormal", "Normal -> size", 1, 3, normal=R(300), reference=R(1))
n(303, "AC1_TilePad", "Normal tile pad", 1, 4, image=R(302), seamless=True, pad_percent=12.5)
n(305, "AC1_MatchNormal", "Normal x4", 3, 3, normal=R(303), reference=R(131))
n(320, "AC1_HeightFromNormals", "Height integrata (riferimento verso)", 4, 3, normal=R(305), opengl=True,
  seamless=False, detail_weight=0.0, height_contrast=1.0, blur_percent=0.0, flatten_percent=3.0)
n(200, "AC1_SystemPrompt", "VLM prompt", 0, 8, setting="medieval")
n(201, "CLIPLoader", "VLM Qwen3-VL 8B", 0, 9, clip_name="qwen3vl_8b_fp8_scaled.safetensors", type="stable_diffusion",
  device="default")
n(202, "TextGenerate", "Analisi materiale (VLM)", 1, 8, clip=R(201), image=R(1), prompt=R(200, 0),
  system_prompt=R(200, 1), max_length=512, sampling_mode="on", **{
      "sampling_mode.temperature": 0.7, "sampling_mode.top_k": 64, "sampling_mode.top_p": 0.95,
      "sampling_mode.min_p": 0.05, "sampling_mode.repetition_penalty": 1.05, "sampling_mode.seed": 0,
      "sampling_mode.presence_penalty": 0, "thinking": False, "use_default_template": True, "mtp": "auto"})
n(203, "AC1_PBRParams", "Materiale (VLM)", 2, 8, llm_reply=R(202))
n(204, "PreviewAny", "Risposta VLM", 3, 8, source=R(203, 1))

# Qwen-Image-Edit 2509 (shared loaders), blueprint "Image Edit (Qwen 2509)" with the 4-step Lightning LoRA
n(400, "UNETLoader", "Qwen-Edit 2509", 4, 10, unet_name="qwen_image_edit_2509_int8_convrot.safetensors",
  weight_dtype="default")
n(401, "LoraLoaderModelOnly", "Lightning 4 steps", 4, 11, model=R(400),
  lora_name="Qwen-Image-Edit-2509-Lightning-4steps-V1.0-bf16.safetensors", strength_model=1.0)
n(402, "ModelSamplingAuraFlow", "shift 3", 4, 12, model=R(401), shift=3.0)
n(403, "CFGNorm", "CFGNorm", 4, 13, model=R(402), strength=1.0)
n(404, "CLIPLoader", "Qwen2.5-VL", 4, 14, clip_name="qwen_2.5_vl_7b_fp8_scaled.safetensors", type="qwen_image",
  device="default")
n(405, "VAELoader", "Qwen VAE", 4, 15, vae_name="qwen_image_vae.safetensors")

# 01 Qwen-Edit from the albedo
n(406, "FluxKontextImageScale", "01 albedo ~1MP", 5, 0, image=R(131))
n(407, "AC1_HeightPrompt", "01 prompt (albedo)", 5, 1, params=R(203), source="albedo")
n(408, "TextEncodeQwenImageEditPlus", "01 +", 5, 2, clip=R(404), prompt=R(407), vae=R(405), image1=R(406))
n(409, "TextEncodeQwenImageEditPlus", "01 -", 5, 3, clip=R(404), prompt="", vae=R(405), image1=R(406))
n(410, "VAEEncode", "01 latent", 5, 4, pixels=R(406), vae=R(405))
n(411, "KSampler", "01 genera", 5, 5, model=R(403), positive=R(408), negative=R(409), latent_image=R(410), seed=0,
  steps=4, cfg=1.0, sampler_name="euler", scheduler="simple", denoise=1.0)
n(412, "VAEDecode", "01 decode", 5, 6, samples=R(411), vae=R(405))

# 02 Qwen-Edit from the game normal map
n(420, "FluxKontextImageScale", "02 normal ~1MP", 6, 0, image=R(303))
n(421, "AC1_HeightPrompt", "02 prompt (normal)", 6, 1, params=R(203), source="normal map")
n(422, "TextEncodeQwenImageEditPlus", "02 +", 6, 2, clip=R(404), prompt=R(421), vae=R(405), image1=R(420))
n(423, "TextEncodeQwenImageEditPlus", "02 -", 6, 3, clip=R(404), prompt="", vae=R(405), image1=R(420))
n(424, "VAEEncode", "02 latent", 6, 4, pixels=R(420), vae=R(405))
n(425, "KSampler", "02 genera", 6, 5, model=R(403), positive=R(422), negative=R(423), latent_image=R(424), seed=0,
  steps=4, cfg=1.0, sampler_name="euler", scheduler="simple", denoise=1.0)
n(426, "VAEDecode", "02 decode", 6, 6, samples=R(425), vae=R(405))

# 03 Flux.2 Klein 4B edit from the albedo (blueprint "Image Edit (Flux.2 Klein 4B)": 20 steps, cfg 5)
n(430, "UNETLoader", "03 Flux.2 Klein 4B", 7, 10, unet_name="flux-2-klein-base-4b-fp8.safetensors",
  weight_dtype="default")
n(431, "CLIPLoader", "03 Qwen3 4B", 7, 11, clip_name="qwen_3_4b.safetensors", type="flux2", device="default")
n(432, "VAELoader", "03 Flux2 VAE", 7, 12, vae_name="flux2-vae.safetensors")
n(433, "ImageScaleToTotalPixels", "03 albedo 1MP", 7, 0, image=R(131), upscale_method="lanczos", megapixels=1.0,
  resolution_steps=1)
n(434, "GetImageSize", "03 size", 7, 1, image=R(433))
n(435, "CLIPTextEncode", "03 +", 7, 2, clip=R(431), text=R(407))
n(436, "CLIPTextEncode", "03 -", 7, 3, clip=R(431), text="")
n(437, "VAEEncode", "03 ref latent", 7, 4, pixels=R(433), vae=R(432))
n(438, "ReferenceLatent", "03 ref +", 7, 5, conditioning=R(435), latent=R(437))
n(439, "ReferenceLatent", "03 ref -", 7, 6, conditioning=R(436), latent=R(437))
n(440, "CFGGuider", "03 guider", 7, 7, model=R(430), positive=R(438), negative=R(439), cfg=5.0)
n(441, "RandomNoise", "03 noise", 7, 8, noise_seed=0)
n(442, "KSamplerSelect", "03 euler", 7, 9, sampler_name="euler")
n(443, "Flux2Scheduler", "03 sigmas", 8, 7, steps=20, width=R(434, 0), height=R(434, 1))
n(444, "EmptyFlux2LatentImage", "03 latent", 8, 8, width=R(434, 0), height=R(434, 1), batch_size=1)
n(445, "SamplerCustomAdvanced", "03 genera", 8, 9, noise=R(441), guider=R(440), sampler=R(442), sigmas=R(443),
  latent_image=R(444))
n(446, "VAEDecode", "03 decode", 8, 10, samples=R(445), vae=R(432))

# 04 PBRify Height (CC0, made for RTX Remix) and 10 PBRify Normal V3 -> integrated
n(450, "UpscaleModelLoader", "04 PBRify Height", 8, 0, model_name="1x-PBRify_Height.pth")
n(451, "AC1_CapSize", "PBRify input 2048", 8, 1, image=R(131), max_side=2048)
n(452, "ImageUpscaleWithModel", "04 height", 8, 2, upscale_model=R(450), image=R(451))
n(490, "UpscaleModelLoader", "10 PBRify NormalV3", 8, 3, model_name="1x-PBRify_NormalV3.pth")
n(491, "ImageUpscaleWithModel", "10 normal", 8, 4, upscale_model=R(490), image=R(451))
n(492, "AC1_HeightFromNormals", "10 integrata", 8, 5, normal=R(491), opengl=True, seamless=False, detail_weight=0.0,
  height_contrast=1.0, blur_percent=0.0, flatten_percent=3.0)

# 05 Depth Anything 3 (mono large)
n(455, "LoadDA3Model", "05 DA3 mono large", 9, 0, model_name="depth_anything_3_mono_large.safetensors",
  weight_dtype="default")
n(456, "DA3Inference", "05 DA3", 9, 1, da3_model=R(455), image=R(131), resolution=1008,
  resize_method="upper_bound_resize", mode="mono")
n(457, "DA3Render", "05 depth", 9, 2, da3_geometry=R(456), output="depth",
  **{"output.normalization": "v2_style", "output.apply_sky_clip": False})

# 06 Lotus depth (blueprint "Image Depth Estimation (Lotus Depth)")
n(460, "UNETLoader", "06 Lotus depth", 10, 0, unet_name="lotus-depth-d-v1-1.safetensors", weight_dtype="default")
n(461, "VAELoader", "06 SD VAE", 10, 1, vae_name="vae-ft-mse-840000-ema-pruned.safetensors")
n(462, "AC1_CapSize", "06 input 1024", 10, 2, image=R(131), max_side=1024)
n(463, "VAEEncode", "06 latent", 10, 3, pixels=R(462), vae=R(461))
n(464, "LotusConditioning", "06 cond", 10, 4)
n(465, "BasicGuider", "06 guider", 10, 5, model=R(460), conditioning=R(464))
n(466, "BasicScheduler", "06 sched", 10, 6, model=R(460), scheduler="normal", steps=1, denoise=1.0)
n(467, "SetFirstSigma", "06 sigma 999", 10, 7, sigmas=R(466), sigma=999.0)
n(468, "DisableNoise", "06 no noise", 10, 8)
n(469, "KSamplerSelect", "06 euler", 10, 9, sampler_name="euler")
n(470, "SamplerCustomAdvanced", "06 depth", 11, 5, noise=R(468), guider=R(465), sampler=R(469), sigmas=R(467),
  latent_image=R(463))
n(471, "VAEDecode", "06 decode", 11, 6, samples=R(470), vae=R(461))
n(472, "ImageInvert", "06 invert", 11, 7, image=R(471))

# 07 MoGe-2 depth
n(475, "LoadMoGeModel", "07 MoGe-2", 11, 0, model_name="moge_2_vitl_normal_fp16.safetensors")
n(476, "AC1_CapSize", "07 input 2048", 11, 1, image=R(131), max_side=2048)
n(477, "MoGeInference", "07 MoGe", 11, 2, moge_model=R(475), image=R(476), resolution_level=9, fov_x_degrees=0.0,
  batch_size=4, force_projection=True, apply_mask=True, refine_steps=3)
n(478, "MoGeRender", "07 depth", 11, 3, moge_geometry=R(477), output="depth")

# 08 DeepBump normal from the colour image -> integrated
n(480, "AC1_DeepBumpNormals", "08 DeepBump normal", 12, 0, image=R(131), model="deepbump256.onnx", overlap="large",
  max_side=2048)
n(481, "AC1_HeightFromNormals", "08 integrata", 12, 1, normal=R(480), opengl=True, seamless=False, detail_weight=0.0,
  height_contrast=1.0, blur_percent=0.0, flatten_percent=3.0)

# 09 hybrid: shape from DA3, fine relief from the game normal map integrated
n(485, "AC1_HeightHybrid", "09 ibrido", 12, 3, coarse=R(457), fine=R(320), fine_weight=0.5)

# finalize (reference size, white = high, 4 px blur) and compare
sources = [R(412), R(426), R(446), R(452), R(457), R(472), R(478), R(481), R(485), R(492)]
for i, src in enumerate(sources, 1):
    # scene-depth models (05 DA3, 06 Lotus, 07 MoGe, 09 hybrid on DA3) see a tilted photo: large undulations removed
    n(500 + i, "AC1_HeightFinalize", f"{i:02} finalize", 13, i - 1, height=src, reference=R(131), blur_px=4.0,
      polarity="auto", flatten_percent=5.0 if i in (5, 6, 7, 9) else 0.0, polarity_reference=R(320))
sheet = dict(albedo=R(131), folder=r"C:\Users\manu\Desktop\MOD\AC1-RTX\pbr\height_lab", texture=R(231), cell=384,
             labels="\n".join(LABELS))
for i in range(1, 11):
    sheet[f"h{i:02}"] = R(500 + i, 0)
    sheet[f"note{i:02}"] = R(500 + i, 1)
n(520, "AC1_HeightLabSheet", "Confronto 10 proposte", 14, 0, **sheet)

# experiments: mix two proposals, then blur (pick the inputs in the UI)
n(530, "AC1_ImageMix", "Mix 2 proposte (A = 01, B = 08)", 15, 0, image_a=R(501), image_b=R(508),
  mode="forma A + dettaglio B", factor=0.5, normalize=True)
n(531, "AC1_ImageBlur", "Sfocatura", 15, 1, image=R(530), radius_px=4.0, mode="gaussiana", seamless=True)
n(532, "PreviewImage", "Anteprima mix", 15, 2, images=R(531))
n(533, "SaveImage", "Salva mix", 15, 4, images=R(531), filename_prefix="ac1_height_mix")


def build_api(main_api):
    api = {}
    for k, v in main_api.items():  # SeedVR2 upscaler subgraph nodes
        if k.startswith("66:"):
            api[k] = v
    api["66:50"]["inputs"].update(image=R(230, 0), alpha=R(230, 1))
    for nid, (cls, inputs, title, _, _) in G.items():
        api[nid] = {"class_type": cls, "inputs": dict(inputs), "_meta": {"title": title}}
    return api


WIDGET_TYPES = {"INT", "FLOAT", "STRING", "BOOLEAN", "COMBO", "COMFY_DYNAMICCOMBO_V3"}


def build_ui(info, main_ui):
    nodes, links = [], []
    by_id = {}
    next_link = [0]
    ids = {}
    for k in G:
        ids[k] = int(k)
    ids["66:59"] = 66
    ui_slot = {}  # (node id, output slot) in UI for API refs
    w66 = next(x for x in main_ui["nodes"] if x["id"] == 66)

    def spec_inputs(cls):
        d = info[cls]
        order = d.get("input_order", {})
        out = []
        for group in ("required", "optional"):
            for name in order.get(group, list(d["input"].get(group, {}))):
                out.append((name, d["input"][group][name], group == "optional"))
        return out

    def is_widget(spec):
        t = spec[0]
        opts = spec[1] if len(spec) > 1 and isinstance(spec[1], dict) else {}
        if opts.get("forceInput"):
            return False
        return isinstance(t, list) or t in WIDGET_TYPES

    for nid, (cls, inputs, title, col, row) in G.items():
        d = info[cls]
        ins, widgets = [], []
        for name, spec, optional in spec_inputs(cls):
            t = spec[0]
            opts = spec[1] if len(spec) > 1 and isinstance(spec[1], dict) else {}
            typ = "COMBO" if isinstance(t, list) else t
            entry = {"localized_name": name, "name": name, "type": typ, "link": None}
            if optional:
                entry["shape"] = 7
            if is_widget(spec):
                entry["widget"] = {"name": name}
                if typ == "COMFY_DYNAMICCOMBO_V3":
                    entry["type"] = "COMBO"
                    value = inputs.get(name, (opts.get("options") or [{}])[0].get("key"))
                    widgets.append(value)
                    for o in opts.get("options", []):
                        if o["key"] == value:
                            for sub, sspec in o["inputs"].get("required", {}).items():
                                widgets.append(inputs.get(f"{name}.{sub}", sspec[1].get("default")))
                else:
                    default = opts.get("default", t[0] if isinstance(t, list) and t else None)
                    v = inputs.get(name, default)
                    if isinstance(v, list):  # linked widget: the frontend keeps a placeholder value
                        v = default if default is not None else ""
                    widgets.append(v)
                    if opts.get("control_after_generate"):
                        widgets.append("fixed")
            ins.append(entry)
        outs = [{"localized_name": oname, "name": oname, "type": otype, "links": []}
                for oname, otype in zip(d.get("output_name", d["output"]), d["output"])]
        node = {"id": ids[nid], "type": cls, "pos": [col * 380, row * 170], "size": [340, 120], "flags": {},
                "order": len(nodes), "mode": 0, "inputs": ins, "outputs": outs,
                "properties": {"Node name for S&R": cls}, "widgets_values": widgets, "title": title}
        nodes.append(node)
        by_id[ids[nid]] = node
    up = json.loads(json.dumps(w66))
    up["pos"] = [2 * 380, 0]
    for i in up["inputs"]:
        i["link"] = None
    for o in up["outputs"]:
        o["links"] = []
    nodes.append(up)
    by_id[66] = up

    def link(src_ref, dst_id, dst_name):
        src, slot = src_ref
        s, d = by_id[ids[src]], by_id[dst_id]
        di = next(i for i, x in enumerate(d["inputs"]) if x["name"] == dst_name)
        next_link[0] += 1
        lid = next_link[0]
        typ = s["outputs"][slot]["type"]
        links.append([lid, s["id"], slot, d["id"], di, typ])
        s["outputs"][slot]["links"].append(lid)
        d["inputs"][di]["link"] = lid

    for nid, (cls, inputs, *_rest) in G.items():
        for name, v in inputs.items():
            if isinstance(v, list) and len(v) == 2 and isinstance(v[0], str) and (v[0] in G or v[0] == "66:59"):
                link(v, ids[nid], name)
    link(["230", 0], 66, "image")
    link(["230", 1], 66, "alpha")
    for x in nodes:
        for i in x["inputs"]:
            if i.get("widget") is None and i["link"] is None and not i.get("shape"):
                pass
    return {"id": "ac1-height-lab", "revision": 0, "last_node_id": max(by_id), "last_link_id": next_link[0],
            "nodes": nodes, "links": links, "groups": [], "definitions": main_ui.get("definitions", {}),
            "config": {}, "extra": {}, "version": 0.4}


def main():
    info = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    missing = sorted({c for c, *_ in G.values()} - set(info))
    if missing:
        sys.exit(f"node types missing from object_info: {missing}")
    api = build_api(json.loads(MAIN_API.read_text(encoding="utf-8")))
    OUT_API.write_text(json.dumps(api, indent=1), encoding="utf-8")
    ui = build_ui(info, json.loads(MAIN_UI.read_text(encoding="utf-8")))
    OUT_UI.write_text(json.dumps(ui, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"{OUT_API.name}: {len(api)} nodes; {OUT_UI.name}: {len(ui['nodes'])} nodes, {len(ui['links'])} links")


if __name__ == "__main__":
    main()
