#include "camera.h"

#include "hook.h"
#include "log.h"

#include <mutex>

namespace ac1rtx::camera {

namespace {

using SetupViewFn = void(__thiscall*)(void* renderer, void* matrixCtx, void* viewSurface, void* viewDesc);
SetupViewFn s_originalSetupView = nullptr;
RenderThreadCallback s_onRenderThread = nullptr;
ViewSetupCallback s_onViewSetup = nullptr;

std::mutex s_mutex;
Snapshot s_frame;
// Main view tracking for the injection point: the widest view of the previous submitted frame, and whether a view
// at least that wide was set up since (Animus menu frames: a 2048-wide view with its own overlay pass precedes the
// 3840-wide scene view).
float s_mainWidth = 0.0f;
bool s_mainViewSeen = false;
bool s_secondaryViewSeen = false;  // a view narrower than the scene view ran this frame (offscreen target producer)

// A frame can set up several views (e.g. a 4:1 offscreen view for a HUD element). The main scene view is
// the one rendering to the widest surface, i.e. the backbuffer-sized DX9ViewSurface.
void __fastcall setupViewHook(void* renderer, void* /*edx*/, void* matrixCtx, void* viewSurface, void* viewDesc) {
  // The engine's own occlusion-culling switch: Remix returns no samples for the engine's occlusion queries,
  // so leaving it on culls almost everything from the colour pass (where materials bind their textures).
  if (viewSurface && game::field<uintptr_t>(viewSurface, 0) == game::kViewSurfaceVtable) {
    auto* culling = static_cast<uint8_t*>(viewSurface) + game::kViewSurfaceOcclusionCulling;
    if (*culling != 0) {
      *culling = 0;
      log::line("view surface %p: engine occlusion culling disabled", viewSurface);
    }
  }
  s_originalSetupView(renderer, matrixCtx, viewSurface, viewDesc);
  s_onRenderThread();

  const float width = game::viewSurfaceWidth(viewSurface);
  // Secondary views (narrower than the previous frame's scene view, e.g. the Animus menu's 2048-wide view whose
  // output textures the menu band) are rasterized as the game draws them; the scene view is ray traced.
  float mainWidth;
  {
    std::lock_guard lock { s_mutex };
    mainWidth = s_mainWidth;
  }
  const bool secondary = mainWidth > 0.0f && width < mainWidth;
  if (s_onViewSetup) {
    s_onViewSetup(secondary);
  }
  std::lock_guard lock { s_mutex };
  s_secondaryViewSeen = s_secondaryViewSeen || secondary;
  if (width >= s_mainWidth) {
    s_mainViewSeen = true;
  }
  if (!s_frame.valid || width > s_frame.surfaceWidth) {
    s_frame.view = game::ctxView(matrixCtx);
    s_frame.projection = game::ctxProjection(matrixCtx);
    s_frame.surfaceWidth = width;
    s_frame.valid = true;
  }
}

} // namespace

bool install(RenderThreadCallback onRenderThread, ViewSetupCallback onViewSetup) {
  s_onRenderThread = onRenderThread;
  s_onViewSetup = onViewSetup;
  return hook::install("Renderer_SetupView", game::kSetupView, game::kSetupViewPrologue,
                       reinterpret_cast<void*>(&setupViewHook), &s_originalSetupView);
}

Snapshot takeFrame() {
  std::lock_guard lock { s_mutex };
  Snapshot result = s_frame;
  if (result.valid) {
    s_mainWidth = result.surfaceWidth;
  }
  s_frame = {};
  s_mainViewSeen = false;
  s_secondaryViewSeen = false;
  return result;
}

bool mainViewSeen() {
  std::lock_guard lock { s_mutex };
  return s_mainViewSeen;
}

bool secondaryViewSeen() {
  std::lock_guard lock { s_mutex };
  return s_secondaryViewSeen;
}

} // namespace ac1rtx::camera
