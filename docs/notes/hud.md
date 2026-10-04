# HUD / 2D overlay on top of the Remix image (design notes)

Status: engine side read from the binary (decompilation), the dx9 trace `frame_capture_01.jsonl` (frames 0 and 1)
and one live read (Masyaf, 2026-10-01). Remix side read from the dxvk-remix source. Implementation built (Remix
runtime + bridge + mod code in `mod/src/hud.{h,cpp}`), **not yet verified in game**.

## 1. How the engine draws its 2D layer

Frame order (decompiled):

```
Renderer_RenderFrame 0xA4BA30
  vtable+4 BeginFrame
  FUN_00A1F4A0            every view: depth prepass, shadow, colour passes, post-process into the back buffer
  vtable+8 = FUN_00A4C510 EndFrame
    FUN_00A1F5A0          (thiscall self, frameParams; ret 4)
      if [frameParams+0x46] == 0 && [self+0x10] == 0:
        draw2d = FUN_00A0C9F0(D, ...)   D = [[self+8]+0x670]; returns D + 0x2B30 (2D drawing interface)
        FUN_009ED5F0(draw2d, frameParams)            <- overlay dispatcher (hooked by the mod)
      SetViewport(0, 0, [frameParams+0xC], [frameParams+0x10])  (FUN_00A0C6B0, device vtable +0xBC)
      FUN_009E6810 (clears fields), FUN_00A0B5E0, FUN_00A1EF10 -> EndScene
    FUN_00A0C380          Present (its only caller is FUN_00A4C510)
```

`FUN_00A1F970` (from 0xA4BCD0 / 0xA4C600) runs the same FUN_00A1F690 / FUN_00A1F4A0 / FUN_00A1F5A0 sequence, so the
dispatcher is the last rendering step of every frame path. It is called only from FUN_00A1F5A0.

`FUN_009ED5F0` (cdecl) runs the end-of-frame callbacks registered through `FUN_009ED380(ctx, fn, extra)`:
array of `{fn, ctx, extra}` at `[0x01A255D8]`, count `[0x01A255DC] & 0x3FFF`, byte `0x01A255D4` = 1 while it runs.
Callback: `void __cdecl fn(ctx, draw2d, frameParams, extra)`.

- Live read (Masyaf, PID 14944): count 1, entry `{0x00786570, 0x01A135A0, 0}`.
- `0x786570` (registered at 0x7865B0) takes the current read buffer of a triple-buffered 2D command queue
  (`FUN_00785FF0`, queue 0x01A135A8) and draws it with `FUN_00786180(draw2d, frameParams)`: 0x70-byte records,
  screen-space vertices scaled from a reference resolution (record +0x68/+0x6A) to the viewport, per-record texture
  (draw2d vtable +0x78), blend mode (+0x6C & 7), depth-mask mode `FUN_00785DF0` (modes 0-3: ZFUNC/ZWRITE/
  COLORWRITEENABLE combinations used for masks, e.g. the minimap).
- Other registrants, all 2D and conditional: SoftBodyStats 0x424DF0, FXStats 0x434550, PhysicStats 0x620610 (debug
  stat overlays), and 0xA641F0 (from the video-frame code 0xC544F0 / 0xC55280) registering 0xA63F70.

D3D9 state of the overlay (frame_capture_01, identical in both frames):

| item | value |
|---|---|
| draws | 181 `DrawPrimitive` TRIANGLELIST per frame (174 quads), all with return address 0x9ED656 in the backtrace |
| position in frame | after the last post-process draw (`DrawPrimitiveUP` into 0x23659420); 0 non-overlay draws after the first overlay draw |
| render target | 0x23659420 = the presented back buffer (no StretchRect/copies after it) |
| depth | DS 0x23659740, `Clear(Z|STENCIL)` from the 2D begin (return 0x78619A) before the first draw |
| shaders | VS 0x18175D20 + PS 0x14D46940, set once; c0-c3 = ortho (2/2560, -2/1440, -1, +1) |
| input | decl 0x21EA4640, stride 24, one dynamic VB; 33 textures in the frame |
| states | ALPHABLEND SRCALPHA/INVSRCALPHA; masks drawn with COLORWRITEENABLE 0 + ZWRITE 1, then ZFUNC LESSEQUAL/GREATER |

## 2. Why the HUD is missing under Remix

- Every game draw uses a vertex shader and `rtx.useVertexCapture = False`, so `D3D9Rtx::makeDrawCallType`
  (d3d9_rtx.cpp, "Skipping draw call with shader usage") ignores it before the UI checks (`isRenderingUI` needs
  either fixed function + ortho or a `rtx.uiTextures` hash).
- No draw ever triggers injection, so Remix injects at Present (`D3D9Rtx::EndFrame` -> `RtxContext::endFrame` ->
  `injectRTX`), after the overlay: the overlay would be overwritten even if it were rasterized.
- `AddTextureHash` / `dxvk_GetTextureHash` are not forwarded by the bridge (client stubs / not in the interface),
  and `SetConfigVariable` values are applied only at the end of `injectRTX` (`RtxOptionManager::applyPendingValues`),
  so neither can mark a point inside a frame.
- The mod submits camera/instances/lights in the bridge Present callback. `RtCamera::isValid(frame)` is
  `m_frameLastTouched == frame`, so an injection earlier in the frame would see no camera unless the frame's world
  is submitted before it.

## 3. Mechanism

1. Mod: hook `FUN_009ED5F0` (prologue `6A FF 68 28 55 56 01 64 A1 00 00 00 00`). On entry it submits the frame's
   camera, geometry and lights (moved from the Present callback; the Present callback keeps doing it when the
   overlay did not run) and calls the new API `InjectRTXAtNextDraw()`.
2. Bridge: `RemixApi_InjectRTXAtNextDraw` command, asynchronous, in the same ordered queue as the D3D9 calls
   (`ProcessDeviceCommandQueue`), so on the server it lands between the last post-process draw and the first overlay
   draw.
3. Remix: `D3D9Rtx::ArmInjectRTXAtNextDraw` (only if `rtx.enableApiInjectionMarker = True`). The next draw returns
   `{Rasterized, trigger injection}` from `makeDrawCallType`, whatever its shaders or state (a mask quad with
   colour writes off would otherwise be ignored). `internalPrepareDraw` binds that draw's render target (the back
   buffer), runs `injectRTX` into it, and `m_rtxInjectTriggered` rasterizes every later draw of the frame
   (existing `PreserveDrawCallAndItsState` path). The mark is cleared in `EndFrame`. Without a following draw,
   injection stays at Present.

Why not texture hashes: the overlay binds 33+ textures per frame (fonts, icons, dynamic ones), the bridge does not
forward `AddTextureHash`, the hash check sits after the vertex-shader rejection, and hash registration is applied only
at frame end. The dispatcher entry is a single code-proven boundary between 3D and 2D.

## 4. Expected limits (to check in game)

- Anything drawn in the 3D views stays invisible (world-space markers, anything the game draws with its 3D passes).
- Overlay draws that sample the game's own scene render targets (e.g. a blurred background behind a menu) read
  targets that Remix never filled (world draws are ignored).
- Loading screens / menus without a game camera: injection finds no valid camera and skips ray tracing, the overlay
  is rasterized over whatever the back buffer holds.
