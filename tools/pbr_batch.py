"""Runs the ComfyUI PBR workflow on every texture the mod dumped (pbr/dump/<hash>.png) and stores the maps where the
mod looks for them (pbr/maps/<hash>/x2/). Everything is local: the ComfyUI server on 127.0.0.1.

Usage (ComfyUI running):
  python tools/pbr_batch.py                 process every dumped texture without maps yet
  python tools/pbr_batch.py --redo          process all of them again (new NNNNN files, the newest wins)

Input per texture: dump/<hash>.png (albedo). The game normal map (dump/<hash>_normal.png) is passed only to a
template that still has the LoadImage node 300 (the height now comes from the AI, 2026-10-02).
The workflow template is tools/pbr_workflow_api.json: the API ("prompt") form of
ComfyUI/user/default/workflows/concept_flussoPBR_AC1remix.json, exported from the ComfyUI frontend (graphToPrompt).
"""

import argparse
import json
import shutil
import sys
import time
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PBR = ROOT / "pbr"
TEMPLATE = Path(__file__).resolve().parent / "pbr_workflow_api.json"
COMFY_INPUT = Path(r"C:\Users\manu\AppData\Local\Comfy-Desktop\ComfyUI-Shared\input")


def request(server, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(server + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def nodes_of(prompt, class_type):
    return [k for k, v in prompt.items() if v["class_type"] == class_type]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8188")
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--variant", default="x4", help="output sub-folder (the workflow upscale factor)")
    ap.add_argument("--only", nargs="*", help="process only these hashes")
    ap.add_argument("--setting", default="medieval", choices=["medieval", "modern (Abstergo)"],
                    help="historical setting given to the VLM, when the template has one (the lab is modern)")
    args = ap.parse_args()

    template = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    NORMAL_LOAD = "300"  # LoadImage of the game normal map (workflow node id)
    load_ids = [k for k in nodes_of(template, "LoadImage") if k != NORMAL_LOAD]
    name_ids = nodes_of(template, "PrimitiveString")
    save_ids = nodes_of(template, "AC1_SaveToFolder")
    if len(load_ids) != 1 or len(name_ids) != 1 or not save_ids:
        sys.exit(f"unexpected template: LoadImage {load_ids}, PrimitiveString {name_ids}, saves {save_ids}")

    from PIL import Image  # ComfyUI's venv has Pillow

    # tiny textures (4x4, 16x16 solid fills) carry no surface to reconstruct
    dumps = [d for d in sorted((PBR / "dump").glob("*.png"))
             if "_" not in d.stem and min(Image.open(d).size) >= 64]
    if args.only:
        dumps = [d for d in dumps if d.stem in set(args.only)]
    todo = [d for d in dumps if args.redo or not list((PBR / "maps" / d.stem / args.variant).glob(f"{d.stem}_albedo_*.png"))]
    print(f"{len(dumps)} dumped textures, {len(todo)} to process")
    client = str(uuid.uuid4())
    for i, src in enumerate(todo, 1):
        name = src.stem
        COMFY_INPUT.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, COMFY_INPUT / f"ac1pbr_{name}.png")
        prompt = json.loads(json.dumps(template))
        prompt[load_ids[0]]["inputs"]["image"] = f"ac1pbr_{name}.png"
        # game normal map dumped by the mod; flat (128,128,255) when the surface has none
        if NORMAL_LOAD in prompt:
            normal_src = PBR / "dump" / f"{name}_normal.png"
            if normal_src.exists():
                shutil.copyfile(normal_src, COMFY_INPUT / f"ac1pbr_{name}_normal.png")
            else:
                Image.new("RGB", Image.open(src).size, (128, 128, 255)).save(COMFY_INPUT / f"ac1pbr_{name}_normal.png")
            prompt[NORMAL_LOAD]["inputs"]["image"] = f"ac1pbr_{name}_normal.png"
        prompt[name_ids[0]]["inputs"]["value"] = name
        for k in nodes_of(prompt, "AC1_SystemPrompt"):
            prompt[k]["inputs"]["setting"] = args.setting
        for s in save_ids + nodes_of(prompt, "AC1_PackForRemix"):  # saves, then BC compression of the same folder
            prompt[s]["inputs"]["folder"] = str(PBR / "maps")
        t0 = time.time()
        pid = request(args.server, "/prompt", {"prompt": prompt, "client_id": client})["prompt_id"]
        while True:
            time.sleep(2)
            hist = request(args.server, f"/history/{pid}")
            if pid in hist:
                status = hist[pid].get("status", {})
                break
        ok = status.get("status_str") == "success"
        print(f"[{i}/{len(todo)}] {name}: {'ok' if ok else 'FAILED'} ({time.time() - t0:.0f} s)")
        if not ok:
            for m in status.get("messages", []):
                if m[0] == "execution_error":
                    print("   ", m[1].get("node_type"), m[1].get("exception_message", "").strip()[:300])


if __name__ == "__main__":
    main()
