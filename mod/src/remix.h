#pragma once

namespace ac1rtx::remix {

struct Settings {
  bool staticMeshes = false;   // submit DX9StaticMesh geometry through the API
  bool lights = false;         // engine lights -> Remix lights, engine sun -> Numos sun angles
  // Calibration, not engine data: the engine has no light size, and its colours light gamma-space albedo.
  float lightSphereRadius = 0.1f;  // metres, radius of the Remix sphere light for omni/spot lights
  float lightRadianceScale = 1.0f; // global engine colour -> Remix radiance factor
};

void configure(const Settings& settings);

// Initialises the Remix API through the bridge client and registers the Present callback.
// Must run on the render thread after the D3D9 device exists; safe to call repeatedly.
void ensureInitialized();

// Engine 2D overlay pass is about to draw (hud::install callback): submits the frame's world now and marks the next
// D3D9 draw as the Remix injection point, so the overlay is rasterized on top of the ray-traced image.
void beginOverlay();

// Engine post-process chain is about to copy its scene into the colour-grading LUT input: with the game post-process
// on (key I) the frame is injected there instead, so the game's LUT applies to the ray-traced image.
void beginPostProcess();

// Engine overlay dispatcher returned: end of the game frame (resets the per-frame injection state).
void endOverlay();

// A surface textured with another view's output is about to be drawn in the scene view: the frame is injected there
// so it (and only the post-process / overlay after it) is rasterized over the ray-traced image.
void beginRenderTargetDraw();

// A view is being set up on the render thread: secondary views' draws into offscreen targets are rasterized by
// Remix (RasterizeOffscreenDraws), the scene view's are not.
void onViewSetup(bool secondaryView);

} // namespace ac1rtx::remix
