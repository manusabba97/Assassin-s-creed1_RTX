# AC1 RTX

Path tracing con RTX Remix per **Assassin's Creed (2008, Director's Cut, versione GOG 1.02, DX9)**, con materiali PBR
generati in locale, parallax, luci, nebbia d'orizzonte e un editor in gioco per materiali e vegetazione.

Il repository contiene solo codice, strumenti e workflow: nessuna texture o asset del gioco. Le mappe PBR si
generano dalla propria copia del gioco con il workflow ComfyUI (cartella [`workflow/`](workflow/)).

## Componenti

| Cartella | Contenuto |
|---|---|
| `mod/` | `AC1RTX.asi` (x86, caricato dall'ASI loader): geometria statica, personaggi, clutter, luci, materiali PBR, HUD, vegetazione via Remix API |
| `tools/` | Pipeline PBR (`pbr_batch.py`, `pbr_pack.py`), piante (`plants_*.py`, `tools/blender/`), vegetazione, deploy e test |
| `workflow/` | Workflow ComfyUI per le mappe PBR e i custom node del progetto |
| `config/rtx.conf` | Impostazioni Remix usate (nebbia d'orizzonte, atmosfera, DLSS) |
| `docs/` | Note tecniche e stato del progetto (`docs/progress.md`) |

Il runtime Remix modificato (editor materiali / vegetazione, nebbia d'orizzonte, blend del terreno, estensioni
dell'API) è nel fork [manusabba97/dxvk-remix](https://github.com/manusabba97/dxvk-remix/tree/ac1-rtx), branch `ac1-rtx`.

## Requisiti

- Assassin's Creed Director's Cut 1.02 (GOG), un ASI loader (es. Ultimate ASI Loader)
- GPU NVIDIA RTX
- Visual Studio 2022 (C++ x86 e x64) per compilare mod e runtime
- Il fork di `dxvk-remix` (runtime + bridge) compilato
- MinHook: `build.bat` lo prende da `Vibe-Reverse-Engineering\rtx_remix_tools\dx\remix-comp-proxy\deps\minhook`
  (cartella accanto a questa)
- Per i materiali: ComfyUI in locale, Python 3 con numpy, Pillow, opencv; Blender 5.x per le piante

## Compilazione e installazione

```bat
mod\build.bat deploy
```

Compila `mod\build\AC1RTX.asi` e lo copia in `<gioco>\scripts\` (il percorso del gioco è in `build.bat`).
Il runtime si installa con `tools\deploy_remix.ps1 -Target "<cartella del gioco>" -Windowed`.
Copiare `config\rtx.conf` nella cartella del gioco e `mod\AC1RTX.ini` in `<gioco>\scripts\`, poi correggere nel file
ini i percorsi (`Folder` della sezione `[PBR]`, `ScreenshotDir`, `ShaderDumpDir`).

## Tasti in gioco

| Tasto | Funzione |
|---|---|
| M | Editor (materiali PBR / vegetazione) |
| L | Blend degli strati del terreno per altezza |
| I | Color grading del gioco (LUT) sopra Remix |
| F7 | Vegetazione originale / vegetazione aggiunta |
| F8 | Nasconde i vetri (diagnostica) |
| F9 | Viste di debug delle normal |
| P | Screenshot |

## Materiali PBR

1. Con `[PBR] Dump=1` la mod salva le texture del livello in `<pbr>\dump`.
2. `tools\pbr_batch.py` le passa al workflow ComfyUI (vedi [`workflow/README.md`](workflow/README.md)).
3. `tools\pbr_pack.py` comprime le mappe (`.ac1t`, BC3/BC5) che la mod carica.

## Vegetazione

- Piante ricostruite in Blender (`tools/blender/plant_builder.py`) da foto di riferimento.
- `tools/veg_painted_pack.py` le prepara per l'editor; in gioco: M → Vegetazione (pennello, gomma, selezione,
  sposta, scala, rotazione, elimina, annulla). Le piante si salvano in `<pbr>\vegetation\painted\placed.txt`.

## Licenza

Codice della mod e strumenti: vedi [`LICENSE`](LICENSE). Assassin's Creed è un marchio di Ubisoft; questo progetto
non è affiliato a Ubisoft né a NVIDIA.
