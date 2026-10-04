#pragma once

#include "game.h"

#include <cstdint>
#include <vector>

namespace ac1rtx::static_geometry {

struct Vertex {
  float position[3];
  float normal[3];
  float texcoord[2];
  float tangent[3];               // engine TANGENT (UBYTE4 @12), same decode as the normal
  float bitangentSign;            // sign(v0.w): the VS computes binormal = sign * cross(N, T)
  uint32_t color = 0xFFFFFFFF;    // D3DCOLOR at +16 (BGRA bytes in memory), 0xFFFFFFFF when the decl has none
  // Terrain layer weights: NORMAL.w and TANGENT.w bytes (layered VS: COLOR1 = (N.w, T.w, 1, 1) / 255).
  uint8_t layerWeight[2] = {};
};

// One DrawIndexedPrimitive range of a DX9StaticMesh, rebased to its own vertex subset, as a triangle list.
// Single-sided surfaces are wound so Remix keeps the faces the engine keeps (D3DCULL_CW ranges reversed, see
// decodeMesh); ranges the engine draws with D3DCULL_NONE are marked double-sided.
struct Surface {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  bool doubleSided = false;
  uint32_t range = 0;             // primitive range index in the DX9StaticMesh
  std::vector<uint32_t> sourceTriangles;  // primitive index within the range for each emitted triangle
  static constexpr uint32_t kNoCell = 0xFFFFFFFFu;
  uint32_t cell = kNoCell;        // masked meshes: the engine cell this surface belongs to
  // Ranges drawn without depth write: engine blend mode (game.h kBlend*, 0 = opaque) and whether the decal pass
  // (render list 1, depth-biased) draws it.
  uint8_t blend = 0;
  bool decal = false;
  // Layered terrain (PS 7a87be5e): 0 = the range itself (base layer), 1/2 = overlay copy for texture layer 1/2 whose
  // opacity is that layer's vertex weight. Ranges with any non-zero weight are kept in their own parts (perRange) so
  // the per-range material decides whether the layers are drawn.
  uint8_t layer = 0;
  bool perRange = false;
};

struct Mesh {
  uint64_t key = 0;               // unique per DX9StaticMesh lifetime (pointer + generation)
  std::vector<Surface> surfaces;
};

struct Instance {
  uint64_t meshKey = 0;
  game::Mat4 world;               // engine row-vector world matrix
  // Masked meshes: per cell, whether the engine draws it this frame. The mask is a distance LOD: near cells are
  // switched off where detailed geometry replaces the far tile (measured: hidden cells 24-182 m, shown 151-506 m).
  std::vector<uint8_t> cellVisible;
};

struct Frame {
  std::vector<Mesh> newMeshes;         // decoded on first sight this frame
  std::vector<uint64_t> destroyedMeshes;
  std::vector<Instance> instances;     // unique (mesh instance, placement) pairs submitted this frame
  uint32_t channelPlacements[8] = {};  // per light-channel bit: unique placements this frame (placement +0x24 >> 3)
};

// Hooks DX9StaticMeshInstance::Submit and the DX9StaticMesh destructor.
bool install();

// Returns and clears everything recorded since the previous call. Render thread only.
Frame takeFrame();

// Engine state of one draw entry (entry +0 MaterialInstance, +0xB range/winding bit, +0xC depth flags):
// 0 not drawn, 1 D3DCULL_NONE, 2 D3DCULL_CW, 3 D3DCULL_CCW, 4 drawn without depth write.
uint8_t engineCull(const uint8_t* entry);

// Registers a DX9StaticMesh drawn by another path (skinned) so its destruction is reported in
// Frame::destroyedMeshes; returns its live key.
uint64_t trackMesh(const void* staticMesh);

// The DX9StaticMeshInstance whose Submit is running on this thread (nullptr outside Submit).
const void* currentMeshInstance();

} // namespace ac1rtx::static_geometry
