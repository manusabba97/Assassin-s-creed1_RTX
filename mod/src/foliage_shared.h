#pragma once
// Vegetation editor channel between the Remix runtime's editor panel (dxvk-remix fork, rtx_fork_foliage.cpp, 64-bit
// bridge server) and the mod (32-bit, in the game): one named shared-memory block "Local\AC1RTX_Foliage" created by
// the mod. Same file in both trees (dxvk-remix/src/dxvk/rtx_render/rtx_fork_foliage_shared.h): keep them identical.
// Only 32-bit fields, packed to 4, so both architectures lay it out the same.
//
// The runtime writes the mouse ray every frame while the editor's vegetation mode is open, and a command by writing
// its fields then incrementing cmdSeq; the mod answers each frame (hover hit, selection, counts) and sets ackSeq once
// a command is applied.
#include <cstdint>

namespace ac1foliage {

constexpr uint32_t kMagic = 0xAC1F0117u;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxSpecies = 32;
constexpr const wchar_t* kMappingName = L"Local\\AC1RTX_Foliage";

enum Command : uint32_t {
  kNone = 0,
  kPaint = 1,           // plants in the brush disc around the ray's ground hit
  kErase = 2,           // removes the plants of the chosen species in the disc
  kSelect = 3,          // the plant under the ray
  kMoveSelected = 4,    // selected plant to the ray's ground hit
  kUpdateSelected = 5,  // selScale / selYaw / selLift
  kDeleteSelected = 6,
  kUndo = 7,
  kClearAll = 8,
  kSave = 9,
  kDeselect = 10,
};

#pragma pack(push, 4)
struct Shared {
  uint32_t magic;
  uint32_t version;
  // runtime -> mod
  uint32_t editorOpen;    // vegetation mode shown: the mod casts the hover ray every frame
  uint32_t cmdSeq;
  uint32_t cmd;
  float rayOrigin[3];
  float rayDir[3];
  float radius;           // brush radius, m
  float density;          // plants per m2 per second of painting
  float scaleMin, scaleMax;
  float slopeAlign;       // 0 upright, 1 perpendicular to the ground
  uint32_t speciesMask;   // bit i = species i may be painted / erased
  float selScale, selYaw, selLift;  // kUpdateSelected (yaw in degrees, lift in m)
  float strokeSeconds;    // time covered by this kPaint (density x area x time plants)
  // mod -> runtime
  uint32_t ackSeq;
  uint32_t hitValid;
  float hit[3];
  float hitNormal[3];
  int32_t selected;       // plant index, -1 none
  uint32_t selSpecies;
  float selPos[3];
  float selScaleOut, selYawOut, selLiftOut, selRadius, selHeight;
  uint32_t plantCount;
  uint32_t undoDepth;
  uint32_t speciesCount;
  char species[kMaxSpecies][40];
  char status[128];
};
#pragma pack(pop)

}  // namespace ac1foliage
