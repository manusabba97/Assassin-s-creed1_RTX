#pragma once

#include <cstdint>
#include <vector>

namespace ac1rtx::skinned {

struct Vertex {
  float position[3];
  float normal[3];
  float texcoord[2];
  uint32_t bones[4];              // palette slots (BLENDINDICES)
  float weights[4];               // BLENDWEIGHT normalised by their sum, as the vertex shader does
};

// One list-0 primitive range of a skinned DX9StaticMesh, decoded once.
struct RangeMesh {
  uint64_t meshKey = 0;           // static_geometry::trackMesh key of the DX9StaticMesh
  uint32_t range = 0;
  uint8_t cull = 0;               // static_geometry::engineCull class the range was decoded for (winding differs)
  bool doubleSided = false;
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

// A skinned range drawn in this frame's colour pass, with the engine's final bone matrices (row-vector, as
// uploaded to c120+).
struct Draw {
  uint64_t meshKey = 0;
  uint32_t range = 0;
  // The entry's MaterialInstance: characters sharing a mesh draw the same range with different MaterialInstances
  // (live Masyaf: 57 of 171 skinned (mesh, range) pairs, 45 meshes), so the Remix mesh/material is per matInst.
  const void* matInst = nullptr;
  uint8_t cull = 0;               // engineCull of this entry: selects the decoded winding
  std::vector<float> bones;       // 12 floats per bone: remixapi_Transform rows (column-vector 3x4)
};

struct Frame {
  std::vector<RangeMesh> newRanges;
  std::vector<Draw> draws;
  uint32_t channelPlacements[8] = {};  // per light-channel bit: unique skinned placements this frame
};

// Hooks the bone-palette builder FUN_00AE11A0.
bool install();

Frame takeFrame();

// True if a skinned palette was ever built for this render object (the Submit `placement`, = render item +0).
bool isSkinnedPlacement(const void* placement);

// The skinned draw entry being drawn on this thread (set by the palette hook, consumed by the material hook).
struct CurrentEntry {
  const void* staticMesh = nullptr;
  const void* matInst = nullptr;
  uint32_t range = 0;
};
bool takeCurrentEntry(const void* matInst, CurrentEntry& out);

// Re-keys this frame's most recent draw of `matInst` (the one just drawn) to `materialKey`: draws whose PS state
// differs per character while sharing a MaterialInstance (skin tone constants) get their own Remix mesh/material.
void setLastDrawKey(const void* matInst, const void* materialKey);

} // namespace ac1rtx::skinned
