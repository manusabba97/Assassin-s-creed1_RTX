#pragma once

#include <cstdint>
#include <vector>

namespace ac1rtx::lights {

enum class Kind : uint8_t { Omni, Spot, Directional, Sun };

// One engine light object (DX9GraphicObjectInstance of type 1) with its scimitar::Light resource, as the engine's
// record builders read them (FUN_00AD49C0 omni, FUN_00AD4AC0 spot, FUN_00AD4010 directional/sun).
struct Light {
  const void* object = nullptr;    // light object L
  const void* resource = nullptr;  // R = [L + 0xA4]
  Kind kind = Kind::Omni;
  uint32_t channels = 0;           // (L+0x24 >> 3) & 0xFF: objects it may light share one of these bits
  float position[3] = {};          // L + 0x70
  float axisY[3] = {};             // L + 0x50: spot/directional lights shine along +Y (shader dir-to-light = -Y)
  float color[3] = {};             // R+0x100 rgb * R+0x10 intensity (fade 255)
  float nearDistance = 0.0f;       // omni/spot
  float farDistance = 0.0f;
  float innerAngle = 0.0f;         // spot half-angles in radians
  float outerAngle = 0.0f;
};

struct Frame {
  bool valid = false;              // the engine ran its light update since the previous takeFrame()
  std::vector<Light> lights;       // the engine's visible light objects (X + 0xA4C)
};

// Hooks the engine's per-view light update (FUN_00AA1AD0).
bool install();

// Returns the latest visible-light snapshot and clears it.
Frame takeFrame();

} // namespace ac1rtx::lights
