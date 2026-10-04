#pragma once

#include "game.h"

namespace ac1rtx::camera {

struct Snapshot {
  game::Mat4 view;
  game::Mat4 projection;
  float surfaceWidth = 0.0f;
  bool valid = false;
};

using RenderThreadCallback = void (*)();
using ViewSetupCallback = void (*)(bool secondaryView);

// Hooks Renderer_SetupView. onRenderThread runs on every view setup, on the game's render thread; onViewSetup then
// tells whether the view is a secondary one (narrower than the previous frame's scene view).
// Returns false if the function bytes do not match the analysed exe.
bool install(RenderThreadCallback onRenderThread, ViewSetupCallback onViewSetup = nullptr);

// Main scene view captured during the frame that is about to be presented; resets the per-frame selection.
Snapshot takeFrame();

// True once this frame set up a view at least as wide as the previous frame's main view (the scene view): the
// Remix injection point must come after it.
bool mainViewSeen();

// True once this frame set up a view narrower than the scene view (e.g. the Animus menu's 2048-wide view that draws
// the band's render target). Without it a render-target textured surface shows a stale target.
bool secondaryViewSeen();

} // namespace ac1rtx::camera
