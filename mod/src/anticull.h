#pragma once

// Visibility and LOD overrides so the engine's draw lists hold the whole loaded world at its most detailed LOD.
// Engine analysis: AC1-RTX/docs/notes/culling_lod.md (addresses and offsets also recorded in kb.h).

namespace ac1rtx::anticull {

struct Settings {
  // Objects and lights outside the camera frustum, and small/medium objects beyond their cull distances, are kept
  // in every view's visible set. Shadow-caster selection, the far-LOD cell swap and gameplay-hidden objects are
  // left to the engine.
  bool antiCulling = false;
  // Every LODSelector draws the LOD the engine selects at distance 0, without LOD cross-fades.
  bool forceMaxLod = false;
  // Tuning (not engine data; engine units = metres). Off-screen objects are kept only within these eye distances,
  // per the engine's size class (FUN_00AC5CE0 flags), and drawn at their coarsest LOD.
  float offscreenRadiusLarge = 150.0f;
  float offscreenRadiusMedium = 40.0f;
  float offscreenRadiusSmall = 0.0f;
  // On-screen small/medium cull distances = engine's x this (0 = no limit).
  float cullDistanceScale = 2.0f;
  // On-screen LOD: most detailed slot within maxLodRadius, beyond it the engine's choice at distance x
  // lodDistanceScale (0.5 = LOD switches twice as far). Characters keep the engine LOD.
  float maxLodRadius = 30.0f;
  float lodDistanceScale = 0.5f;
};

// Installs the hooks the enabled switches need. Requires MH_Initialize.
bool install(const Settings& settings);

} // namespace ac1rtx::anticull
