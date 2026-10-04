#pragma once

namespace ac1rtx::hud {

// Hooks the engine's end-of-frame 2D overlay dispatcher (FUN_009ED5F0). `onOverlayBegin` runs on the render thread
// once per presented frame, after every 3D view of the frame was drawn and right before the game's own 2D layer
// (HUD, menus, subtitles, loading-screen 2D) issues its first D3D9 call. Details: AC1-RTX/docs/notes/hud.md.
//
// Also hooks the view's post-process chain (FUN_00AAF540 called from 0xAC02E1): `onPostProcessBegin` runs right
// before its first draw, the copy of the scene into the colour-grading LUT's input (see hud.cpp).
// `onOverlayEnd` runs after the dispatcher returns: the game frame's last render-thread step before Present.
// `onRenderTargetDraw` runs right before the colour-pass draw of a surface textured with a render target (another
// view's output, e.g. the Animus menu band), which is rasterized over the ray-traced image.
bool install(void (*onOverlayBegin)(), void (*onOverlayEnd)(), void (*onPostProcessBegin)(),
             void (*onRenderTargetDraw)());

} // namespace ac1rtx::hud
