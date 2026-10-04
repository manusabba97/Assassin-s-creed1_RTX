# Workflow PBR (ComfyUI)

Genera le mappe PBR (albedo, normal, ruvidità, metallo, altezza + parametri del materiale) da ogni texture del
gioco che la mod salva in `<pbr>\dump`. Tutto in locale: nessun nodo API o cloud.

| File | Uso |
|---|---|
| `AC1_PBR_workflow.json` | Workflow da aprire in ComfyUI (trascinarlo nella finestra) |
| `AC1_height_lab.json` | Banco di prova delle height map (10 proposte a confronto) |
| `../tools/pbr_workflow_api.json` | Lo stesso workflow in formato API, usato da `tools/pbr_batch.py` |
| `../tools/height_lab_api.json` | Formato API del banco di prova (`tools/height_lab.py`) |
| `comfyui/ComfyUI-AC1-PBR` | Custom node del progetto (nodi `AC1_*`) |
| `comfyui/RGBAtoRGB` | Nodo che toglie il canale alpha |

## Installazione

1. ComfyUI recente (provato con 0.38.1). I nodi SeedVR2, Depth Anything 3, MoGe, Lotus e TextGenerate sono nel core.
2. Copiare le due cartelle di `comfyui/` in `ComfyUI/custom_nodes/`.
3. Custom node esterni:
   - [ComfyUI-PBRFusion4](https://github.com/Night1099/COMFYUI-PBRFusion4) (profondità `AIXPOLY_PBRFusion4…`)
   - [ComfyUI-RMBG](https://github.com/1038lab/ComfyUI-RMBG) (BiRefNet, solo per le piante)
4. Dipendenze Python dei nodi `AC1_*`: `opencv-python`, `numpy`, `torch` (già in ComfyUI), `diffusers` (Marigold).
5. Modelli, nelle cartelle di `ComfyUI/models/`:

| Modello | Cartella | Usato da |
|---|---|---|
| `qwen3vl_8b_fp8_scaled.safetensors` | `text_encoders` | analisi del materiale (VLM) |
| `seedvr2_7b_int8_convrot.safetensors` | `diffusion_models` | upscale SeedVR2 |
| `seedvr2_ema_vae_fp16.safetensors` | `vae` | upscale SeedVR2 |
| `depth_anything_3_mono_large.safetensors` | `geometry_estimation` | profondità |
| `1x-PBRify_NormalV3.pth` | `upscale_models` | normal |
| `prs-eth/marigold-iid-appearance-v1-1` | cache Hugging Face (scaricato al primo uso) | ruvidità / metallo (Marigold) |

Solo per il banco di prova delle altezze, in più:

| Modello | Cartella |
|---|---|
| `qwen_image_edit_2509_int8_convrot.safetensors`, `flux-2-klein-base-4b-fp8.safetensors`, `lotus-depth-d-v1-1.safetensors` | `diffusion_models` |
| `qwen_2.5_vl_7b_fp8_scaled.safetensors`, `qwen_3_4b.safetensors` | `text_encoders` |
| `qwen_image_vae.safetensors`, `flux2-vae.safetensors`, `vae-ft-mse-840000-ema-pruned.safetensors` | `vae` |
| `Qwen-Image-Edit-2509-Lightning-4steps-V1.0-bf16.safetensors` | `loras` |
| `moge_2_vitl_normal_fp16.safetensors` | `geometry_estimation` |
| `1x-PBRify_Height.pth` | `upscale_models` |
| `deepbump256.onnx` | `deepbump` |

## Uso

1. Gioco con `[PBR] Dump=1` nell'ini: girare il livello, le texture finiscono in `<pbr>\dump`.
2. ComfyUI avviato, poi:
   ```bat
   python tools\pbr_batch.py --server http://127.0.0.1:8188
   python tools\pbr_pack.py
   ```
3. In gioco le mappe si regolano con l'editor (tasto M).

Percorsi da adattare alla propria macchina: `COMFY_INPUT` in `tools/pbr_batch.py`, e nel workflow i campi
`folder` / `pack_tool` dei nodi di salvataggio.
