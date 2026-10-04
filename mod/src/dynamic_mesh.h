#pragma once

#include "game.h"

#include <cstdint>
#include <vector>

namespace ac1rtx::dynamic_mesh {

// One colour-pass draw entry of a scimitar::DX9DynamicSubMeshInstance.
// CPU-vertex path (cloth: robes, awnings): vertices are the engine's CPU array for this frame (the simulation output
// copied to the ring VB by FUN_00A0CAD0), drawn once with `world`.
// Instanced path (clutter: vegetation, small props; game.h kDynamicMatrices): one copy of the mesh in object space,
// drawn once per entry of `instances`; the geometry does not change, so it is identified by geometryHash.
struct Draw {
  const void* resource = nullptr;   // dynamic mesh R = [instance + 0x1C]
  const void* matInst = nullptr;
  game::Mat4 world;                 // item + 0x100 (0xEA, g_World) at the draw (cloth)
  bool doubleSided = false;         // engine D3DCULL_NONE
  struct Vertex {
    float position[3];
    float normal[3];
    float texcoord[2];
  };
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;    // triangle list, wound for Remix (engine D3DCULL_CW reversed)
  struct Transform {
    float m[3][4];                  // column-vector 3x4, rows = world x / y / z (remixapi_Transform layout)
  };
  std::vector<Transform> instances;  // empty for cloth
  uint64_t geometryHash = 0;         // instanced path only
};

struct Frame {
  std::vector<Draw> draws;
};

// Hooks DX9DynamicSubMeshInstance::Submit (0xAA38A0) and the dynamic mesh draw (0xAA4780).
bool install();

// Called from MaterialInstance_EndDraw: completes the pending draw of this thread with its MaterialInstance.
// Returns the dynamic mesh resource when the ending draw was one of ours (material key = (resource, 0, matInst)).
const void* onEndDraw(const void* matInst);

Frame takeFrame();

} // namespace ac1rtx::dynamic_mesh
