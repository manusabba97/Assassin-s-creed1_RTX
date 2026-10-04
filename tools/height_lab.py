"""Runs the height lab (10 height-map proposals, tools/height_lab_api.json) on dumped textures; everything local.

Usage (ComfyUI running):
  python tools/height_lab.py --only 326AE55313C5573F 25AE17DFD717427F [--server http://127.0.0.1:8188]

Inputs per texture: pbr/dump/<hash>.png and pbr/dump/<hash>_normal.png (flat normal when missing).
Output: pbr/height_lab/<hash>/NN_<method>.png (16-bit) and sheet.png (all proposals, height + lit relief).
"""

import argparse
import json
import shutil
import time
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PBR = ROOT / "pbr"
TEMPLATE = Path(__file__).resolve().parent / "height_lab_api.json"
COMFY_INPUT = Path(r"C:\Users\manu\AppData\Local\Comfy-Desktop\ComfyUI-Shared\input")


def request(server, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(server + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8188")
    ap.add_argument("--only", nargs="+", required=True)
    args = ap.parse_args()
    from PIL import Image
    template = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    client = str(uuid.uuid4())
    for i, name in enumerate(args.only, 1):
        src = PBR / "dump" / f"{name}.png"
        shutil.copyfile(src, COMFY_INPUT / f"ac1pbr_{name}.png")
        normal = PBR / "dump" / f"{name}_normal.png"
        if normal.exists():
            shutil.copyfile(normal, COMFY_INPUT / f"ac1pbr_{name}_normal.png")
        else:
            Image.new("RGB", Image.open(src).size, (128, 128, 255)).save(COMFY_INPUT / f"ac1pbr_{name}_normal.png")
        prompt = json.loads(json.dumps(template))
        prompt["1"]["inputs"]["image"] = f"ac1pbr_{name}.png"
        prompt["300"]["inputs"]["image"] = f"ac1pbr_{name}_normal.png"
        prompt["231"]["inputs"]["value"] = name
        prompt["520"]["inputs"]["folder"] = str(PBR / "height_lab")
        t0 = time.time()
        pid = request(args.server, "/prompt", {"prompt": prompt, "client_id": client})["prompt_id"]
        while True:
            time.sleep(3)
            hist = request(args.server, f"/history/{pid}")
            if pid in hist:
                status = hist[pid].get("status", {})
                break
        ok = status.get("status_str") == "success"
        print(f"[{i}/{len(args.only)}] {name}: {'ok' if ok else 'FAILED'} ({time.time() - t0:.0f} s)")
        if not ok:
            for m in status.get("messages", []):
                if m[0] == "execution_error":
                    print("   ", m[1].get("node_id"), m[1].get("node_type"),
                          m[1].get("exception_message", "").strip()[:500])


if __name__ == "__main__":
    main()
