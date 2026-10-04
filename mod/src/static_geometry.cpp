#include "static_geometry.h"

#include "hook.h"
#include "log.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ac1rtx::static_geometry {

namespace {

using SubmitFn = void(__thiscall*)(void* meshInstance, void* renderItem, void* matrixCtx, void* placement);
using DestructorFn = void(__thiscall*)(void* staticMesh);
SubmitFn s_originalSubmit = nullptr;
SubmitFn s_originalMaskedSubmit = nullptr;
using SkyPassFn = void(__thiscall*)(void* pass, void* list, void* a2, void* a3, void* a4, void* a5);
SkyPassFn s_originalSkyPass = nullptr;
thread_local bool t_inSkyPass = false;
thread_local const void* t_currentInstance = nullptr;
std::unordered_set<const void*> s_skyMeshes;  // DX9StaticMesh* drawn by the background pass
DestructorFn s_originalDestructor = nullptr;

std::mutex s_mutex;
std::unordered_map<const void*, uint64_t> s_liveMeshes;     // DX9StaticMesh* -> key
std::unordered_set<const void*> s_rejectedMeshes;
struct DeclInfo {
  bool valid = false;
  int colorOffset = -1;  // D3DDECLUSAGE_COLOR (D3DCOLOR) in stream 0, -1 when absent
};
std::unordered_map<const void*, DeclInfo> s_declValid;       // IDirect3DVertexDeclaration9* -> decoder layout
uint32_t s_generation = 0;
Frame s_frame;
std::unordered_set<uint64_t> s_frameInstances;

// D3DRS_CULLMODE the engine applies to a primitive range in the main pass, or kNotDrawn when no draw entry
// references the range (or its material is flagged never-drawn), or kNoDepthWrite when the engine draws it
// without writing depth (the engineCull() encoding shared with the skinned path).
enum Cull : uint8_t { kNotDrawn = 0, kCullNone = 1, kCullCW = 2, kCullCCW = 3, kNoDepthWrite = 4 };

// Everything FUN_00A4A580 derives from one draw entry for the colour pass.
struct RangeState {
  Cull cull = kNotDrawn;     // winding (kCullNone/CW/CCW), also for ranges drawn without depth write
  bool depthWrite = true;
  uint8_t blend = 0;         // material flags & 7 (game.h kBlend*)
  uint8_t lists = 0;         // entry +0xA: render lists that draw the entry (bit 1 = decal pass)

  bool operator==(const RangeState& o) const {
    return cull == o.cull && depthWrite == o.depthWrite && blend == o.blend && lists == o.lists;
  }
};

RangeState stateOfEntry(const uint8_t* entry) {
  RangeState s;
  const uint8_t rangeByte = entry[game::kDrawEntryRange];
  const bool entryForced = (rangeByte & 0x80) != 0;
  const void* matInst = game::field<const void*>(entry, game::kDrawEntryMaterialInstance);
  const void* material = matInst ? game::field<const void*>(matInst, game::kMaterialInstanceMaterial) : nullptr;
  if (!material) {
    return s;
  }
  const uint32_t flags = game::field<uint32_t>(material, game::kMaterialFlags);
  if (flags & game::kMaterialHidden) {
    return s;
  }
  s.blend = static_cast<uint8_t>(flags & game::kMaterialBlendMask);
  s.lists = entry[game::kDrawEntryLists];
  const uint8_t depthFlags = entry[game::kDrawEntryDepthFlags];
  const uint32_t noDepthWriteBit = s.blend == 0 ? game::kMaterialNoDepthWriteOpaque
                                                : game::kMaterialNoDepthWriteBlended;
  s.depthWrite = (depthFlags & 1) ? true : (depthFlags & 2) ? false : (flags & noDepthWriteBit) == 0;
  const bool instanceForced = (game::field<uint8_t>(matInst, game::kMaterialInstanceFlags) & 1) != 0;
  if (instanceForced || entryForced) {
    s.cull = entryForced ? kCullCCW : kCullCW;
  } else if (flags & game::kMaterialNoCull) {
    s.cull = kCullNone;
  } else {
    s.cull = (flags & game::kMaterialCullCCW) ? kCullCCW : kCullCW;
  }
  return s;
}

Cull cullOfEntry(const uint8_t* entry) {
  const RangeState s = stateOfEntry(entry);
  return (s.cull != kNotDrawn && !s.depthWrite) ? kNoDepthWrite : s.cull;
}

std::vector<RangeState> readRangeStates(const void* meshInstance, uint32_t rangeCount) {
  std::vector<RangeState> states(rangeCount);
  const auto* entries = game::field<const uint8_t*>(meshInstance, game::kMeshInstanceDrawEntries);
  const uint32_t entryCount = game::field<uint32_t>(meshInstance, game::kMeshInstanceDrawEntryCount) & 0x3FFF;
  for (uint32_t e = 0; entries && e < entryCount; ++e) {
    const uint8_t* entry = entries + e * game::kDrawEntrySize;
    const uint32_t range = entry[game::kDrawEntryRange] & 0x7F;
    if (range < rangeCount && states[range].cull == kNotDrawn) {
      states[range] = stateOfEntry(entry);
    }
  }
  return states;
}

struct StreamLayout {
  int positionOffset = -1;
  int normalOffset = -1;
  int texcoordOffset = -1;
  int tangentOffset = -1;
  int colorOffset = -1;
};

// The decoder implements exactly the layout the static-mesh vertex shaders consume; any declaration that
// differs is rejected instead of being reinterpreted.
DeclInfo validateDeclaration(IDirect3DVertexDeclaration9* decl) {
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1] = {};
  UINT count = MAXD3DDECLLENGTH + 1;
  if (FAILED(decl->GetDeclaration(elements, &count))) {
    log::line("GetDeclaration failed for decl %p", decl);
    return {};
  }
  StreamLayout layout;
  for (UINT i = 0; i < count && elements[i].Stream != 0xFF; ++i) {
    const auto& e = elements[i];
    if (e.Stream != 0 || e.UsageIndex != 0) {
      continue;
    }
    if (e.Usage == D3DDECLUSAGE_POSITION && e.Type == D3DDECLTYPE_SHORT4) {
      layout.positionOffset = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_NORMAL && e.Type == D3DDECLTYPE_UBYTE4) {
      layout.normalOffset = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.Type == D3DDECLTYPE_SHORT2N) {
      layout.texcoordOffset = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TANGENT && e.Type == D3DDECLTYPE_UBYTE4) {
      layout.tangentOffset = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_COLOR && e.Type == D3DDECLTYPE_D3DCOLOR) {
      layout.colorOffset = e.Offset;
    }
  }
  const bool valid = layout.positionOffset == 0 && layout.normalOffset == 8 && layout.texcoordOffset == 20;
  log::line("vertex decl %p: pos@%d normal@%d uv@%d tangent@%d color@%d -> %s", decl, layout.positionOffset,
            layout.normalOffset, layout.texcoordOffset, layout.tangentOffset, layout.colorOffset,
            valid ? "accepted" : "REJECTED");
  return { valid, layout.colorOffset };
}

Vertex decodeVertex(const uint8_t* v, int colorOffset) {
  const auto* p = reinterpret_cast<const int16_t*>(v);
  const float scale = std::fabs(p[3] * game::kPositionScale);
  Vertex out;
  out.position[0] = p[0] * scale;
  out.position[1] = p[1] * scale;
  out.position[2] = p[2] * scale;

  float n[3];
  for (int i = 0; i < 3; ++i) {
    n[i] = (v[8 + i] - game::kNormalBias) * game::kNormalScale;
  }
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  for (int i = 0; i < 3; ++i) {
    out.normal[i] = len > 0.0f ? n[i] / len : 0.0f;
  }

  const auto* uv = reinterpret_cast<const int16_t*>(v + 20);
  out.texcoord[0] = (uv[0] / 32767.0f) * game::kTexcoordScale;
  out.texcoord[1] = (uv[1] / 32767.0f) * game::kTexcoordScale;

  float t[3];
  for (int i = 0; i < 3; ++i) {
    t[i] = (v[12 + i] - game::kNormalBias) * game::kNormalScale;
  }
  const float tlen = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
  for (int i = 0; i < 3; ++i) {
    out.tangent[i] = tlen > 0.0f ? t[i] / tlen : 0.0f;
  }
  out.bitangentSign = p[3] < 0 ? -1.0f : 1.0f;
  out.layerWeight[0] = v[11];
  out.layerWeight[1] = v[15];
  if (colorOffset >= 0) {
    out.color = *reinterpret_cast<const uint32_t*>(v + colorOffset);
  }
  return out;
}

template<typename Buffer, typename Desc>
const uint8_t* lockReadOnly(Buffer* buffer, Desc& desc) {
  if (FAILED(buffer->GetDesc(&desc))) {
    return nullptr;
  }
  void* data = nullptr;
  if (FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) {
    return nullptr;
  }
  return static_cast<const uint8_t*>(data);
}

bool buildSurface(const game::PrimitiveRange& range, const uint8_t* vertices, uint32_t vertexCount, uint32_t stride,
                  const uint8_t* indices, uint32_t indexCount, bool index32, bool reverseWinding, int colorOffset,
                  Surface& out) {
  auto indexAt = [&](uint32_t i) -> uint32_t {
    return index32 ? reinterpret_cast<const uint32_t*>(indices)[i] : reinterpret_cast<const uint16_t*>(indices)[i];
  };
  const uint32_t first = range.startIndex;
  const uint32_t count = range.type == D3DPT_TRIANGLELIST ? range.primitiveCount * 3 : range.primitiveCount + 2;
  if ((range.type != D3DPT_TRIANGLELIST && range.type != D3DPT_TRIANGLESTRIP) || first + count > indexCount ||
      range.minVertex + range.numVertices > vertexCount) {
    log::line("primitive range rejected: type=%u min=%u num=%u start=%u prims=%u (vb=%u ib=%u)", range.type,
              range.minVertex, range.numVertices, range.startIndex, range.primitiveCount, vertexCount, indexCount);
    return false;
  }

  out.vertices.reserve(range.numVertices);
  for (uint32_t v = 0; v < range.numVertices; ++v) {
    out.vertices.push_back(decodeVertex(vertices + (range.minVertex + v) * stride, colorOffset));
  }

  uint32_t currentTriangle = 0;
  auto emit = [&](uint32_t a, uint32_t b, uint32_t c) -> bool {
    for (uint32_t idx : { a, b, c }) {
      if (idx < range.minVertex || idx >= range.minVertex + range.numVertices) {
        return false;
      }
    }
    if (a == b || b == c || a == c) {
      return true; // degenerate strip joint
    }
    if (reverseWinding) {
      std::swap(b, c);
    }
    out.indices.insert(out.indices.end(), { a - range.minVertex, b - range.minVertex, c - range.minVertex });
    out.sourceTriangles.push_back(currentTriangle);
    return true;
  };
  for (uint32_t t = 0; t < range.primitiveCount; ++t) {
    currentTriangle = t;
    bool ok;
    if (range.type == D3DPT_TRIANGLELIST) {
      ok = emit(indexAt(first + t * 3), indexAt(first + t * 3 + 1), indexAt(first + t * 3 + 2));
    } else if (t & 1) {
      ok = emit(indexAt(first + t + 1), indexAt(first + t), indexAt(first + t + 2));
    } else {
      ok = emit(indexAt(first + t), indexAt(first + t + 1), indexAt(first + t + 2));
    }
    if (!ok) {
      log::line("index outside primitive range vertex window; surface rejected");
      return false;
    }
  }
  return !out.indices.empty();
}

bool decodeMesh(const void* staticMesh, const std::vector<RangeState>& states, IDirect3DVertexDeclaration9* decl,
                Mesh& out) {
  auto declIt = s_declValid.find(decl);
  if (declIt == s_declValid.end()) {
    declIt = s_declValid.emplace(decl, validateDeclaration(decl)).first;
  }
  if (!declIt->second.valid) {
    return false;
  }
  const int colorOffset = declIt->second.colorOffset;

  const uint32_t stride = game::field<uint32_t>(staticMesh, game::kStaticMeshStride) & 0x7FF;
  const void* vbWrapper = game::field<const void*>(staticMesh, game::kStaticMeshVertexBuffer);
  const void* ibWrapper = game::field<const void*>(staticMesh, game::kStaticMeshIndexBuffer);
  if (!vbWrapper || !ibWrapper || stride != 24) {
    log::line("static mesh %p: vb=%p ib=%p stride=%u - skipped", staticMesh, vbWrapper, ibWrapper, stride);
    return false;
  }
  auto* vb = game::field<IDirect3DVertexBuffer9*>(vbWrapper, game::kDeviceBufferD3D);
  auto* ib = game::field<IDirect3DIndexBuffer9*>(ibWrapper, game::kDeviceBufferD3D);

  D3DVERTEXBUFFER_DESC vbDesc = {};
  D3DINDEXBUFFER_DESC ibDesc = {};
  const uint8_t* vertices = lockReadOnly(vb, vbDesc);
  if (!vertices) {
    log::line("static mesh %p: vertex buffer lock failed", staticMesh);
    return false;
  }
  const uint8_t* indices = lockReadOnly(ib, ibDesc);
  if (!indices) {
    vb->Unlock();
    log::line("static mesh %p: index buffer lock failed", staticMesh);
    return false;
  }
  const bool index32 = ibDesc.Format == D3DFMT_INDEX32;
  const uint32_t vertexCount = vbDesc.Size / stride;
  const uint32_t indexCount = ibDesc.Size / (index32 ? 4 : 2);

  const auto* ranges = game::field<const game::PrimitiveRange*>(staticMesh, game::kStaticMeshPrimitiveTable);
  for (uint32_t r = 0; r < states.size(); ++r) {
    const RangeState& st = states[r];
    if (st.cull == kNotDrawn) {
      continue;
    }
    if (!st.depthWrite) {
      // Drawn without depth write: blended surfaces and decals. Remix equivalents exist for the engine's alpha and
      // multiplicative modes (rtx_instance_manager.cpp:738-741, 810-815); the others are reported and not sent.
      const bool supported = st.blend == game::kBlendAlpha || st.blend == game::kBlendMultiply ||
                             st.blend == game::kBlendGlass;  // glass: Remix translucent (remix.cpp)
      static int logged = 0;
      if (logged++ < 40) {
        log::line("mesh %p range %u without depth write: blend %u lists %02X cull %u (verts=%u tris=%u) -> %s",
                  staticMesh, r, st.blend, st.lists, st.cull, ranges[r].numVertices, ranges[r].primitiveCount,
                  supported ? (st.lists & game::kDecalListBit ? "decal" : "blended") : "not submitted");
      }
      if (!supported) {
        continue;
      }
    }
    // The engine camera is right-handed (det(view3x3) = +1, projection w = -z_view), so Remix sees
    // viewToProjection * worldToView as mirrored and flips facing (rtx_instance_manager.cpp:59-81): a single-sided
    // API instance then keeps what D3D9 keeps under D3DCULL_CCW. The engine's D3DCULL_CW ranges are therefore
    // reversed; CCW ranges stay as authored. Verified with rtx.debugView.debugViewIdx = 3 (Is Front Hit):
    // unchanged CW ranges were all back hits.
    Surface surface;
    surface.doubleSided = st.cull == kCullNone;
    surface.range = r;
    if (!st.depthWrite) {
      surface.blend = st.blend;
      surface.decal = (st.lists & game::kDecalListBit) != 0;
      surface.perRange = true;  // own part: its PS tint/alpha constants are per range (remix.cpp drawStatic)
    }
    if (buildSurface(ranges[r], vertices, vertexCount, stride, indices, indexCount, index32, st.cull == kCullCW,
                     colorOffset, surface)) {
      // Opaque ranges with layer weights: add overlay copies for the layered terrain shader (drawn only when the
      // range's material turns out to be layered, see remix.cpp).
      bool weights[2] = {};
      if (st.depthWrite) {
        for (const Vertex& v : surface.vertices) {
          weights[0] |= v.layerWeight[0] != 0;
          weights[1] |= v.layerWeight[1] != 0;
        }
      }
      if (weights[0] || weights[1]) {
        surface.perRange = true;
        for (uint8_t layer = 1; layer <= 2; ++layer) {
          if (weights[layer - 1]) {
            Surface overlay = surface;
            overlay.layer = layer;
            out.surfaces.push_back(std::move(overlay));
          }
        }
      }
      out.surfaces.push_back(std::move(surface));
    }
  }  ib->Unlock();
  vb->Unlock();
  return !out.surfaces.empty();
}

// ---- Masked-mesh cells (layout in game.h, from FUN_00ADE5B0).
struct CellTable {
  uint32_t first = 0;
  uint32_t count = 0;
  const uint8_t* perRange = nullptr;  // [range*8] -> cell array {int start, int count}
  const uint8_t* bits = nullptr;
};

bool readCellTable(const void* item, const void* staticMesh, CellTable& t) {
  const void* object = game::field<const void*>(item, game::kRenderItemObject);
  const uint8_t* h = object ? game::field<const uint8_t*>(object, 0x60) : nullptr;
  if (!h || !(game::field<uint32_t>(h, 8) & 0x10000000)) {
    return false;
  }
  const void* o1 = game::field<const void*>(h, 0);
  const uint8_t* h2 = o1 ? game::field<const uint8_t*>(o1, 0x1C) : nullptr;
  if (!h2 || !(game::field<uint32_t>(h2, 8) & 0x10000000)) {
    return false;
  }
  const void* o2 = game::field<const void*>(h2, 0);
  const uint8_t* a = o2 ? game::field<const uint8_t*>(o2, 0xC) : nullptr;
  if (!a || game::field<uint32_t>(a, 8) == 0) {
    return false;
  }
  t.first = (game::field<uint32_t>(staticMesh, game::kStaticMeshCellInfo) >> 12) & 0xFFF;
  t.count = game::field<uint8_t>(staticMesh, game::kStaticMeshCellCount);
  const uint8_t* group = a + (t.first / game::field<uint32_t>(a, 8)) * 0x14;
  if ((game::field<uint32_t>(group, 0x10) & 0x3FFF) == 0) {
    return false;
  }
  t.perRange = game::field<const uint8_t*>(group, 0xC);
  const void* owner = game::field<const void*>(item, game::kRenderItemCellOwner);
  t.bits = owner ? game::field<const uint8_t*>(owner, game::kCellVisibilityBits) : nullptr;
  return t.perRange && t.bits && t.count > 0;
}

bool cellVisible(const CellTable& t, uint32_t k) {
  const uint32_t id = t.first + k;
  return ((t.bits[id >> 3] >> (7 - (id & 7))) & 1) != 0;
}

// Splits each surface of a masked mesh into one surface per engine cell (triangles outside every cell are never
// drawn by FUN_00ADE5B0 and are dropped), so Remix can draw exactly the cells the engine draws each frame.
void splitByCells(const CellTable& t, Mesh& mesh) {
  std::vector<Surface> out;
  uint32_t dropped = 0;
  for (auto& s : mesh.surfaces) {
    const uint8_t* cellArray = game::field<const uint8_t*>(t.perRange, s.range * 8);
    if (!cellArray) {
      dropped += static_cast<uint32_t>(s.sourceTriangles.size());
      continue;
    }
    std::vector<Surface> parts(t.count);
    std::vector<std::vector<uint32_t>> remap(t.count);
    for (size_t tri = 0; tri < s.sourceTriangles.size(); ++tri) {
      const int index = static_cast<int>(s.sourceTriangles[tri] * 3);
      uint32_t k = 0;
      for (; k < t.count; ++k) {
        const int start = game::field<int>(cellArray, k * 8);
        const int count = game::field<int>(cellArray, k * 8 + 4);
        if (index >= start && index < start + count) {
          break;
        }
      }
      if (k == t.count) {
        ++dropped;
        continue;
      }
      Surface& part = parts[k];
      auto& map = remap[k];
      if (map.empty()) {
        map.assign(s.vertices.size(), 0xFFFFFFFFu);
      }
      for (int corner = 0; corner < 3; ++corner) {
        const uint32_t v = s.indices[tri * 3 + corner];
        if (map[v] == 0xFFFFFFFFu) {
          map[v] = static_cast<uint32_t>(part.vertices.size());
          part.vertices.push_back(s.vertices[v]);
        }
        part.indices.push_back(map[v]);
      }
      part.sourceTriangles.push_back(s.sourceTriangles[tri]);
    }
    for (uint32_t k = 0; k < t.count; ++k) {
      if (!parts[k].indices.empty()) {
        parts[k].doubleSided = s.doubleSided;
        parts[k].blend = s.blend;
        parts[k].decal = s.decal;
        parts[k].layer = s.layer;
        parts[k].perRange = s.perRange;
        parts[k].range = s.range;
        parts[k].cell = k;
        out.push_back(std::move(parts[k]));
      }
    }
  }
  if (dropped) {
    log::line("masked mesh: %u triangles outside every cell dropped", dropped);
  }
  mesh.surfaces = std::move(out);
}

void recordInstance(void* meshInstance, void* placement, bool masked, const void* item) {
  const void* staticMesh = game::field<const void*>(meshInstance, game::kMeshInstanceStaticMesh);
  auto* decl = game::field<IDirect3DVertexDeclaration9*>(meshInstance, game::kMeshInstanceVertexDecl);
  if (!staticMesh || !decl || !placement) {
    return;
  }

  std::lock_guard lock { s_mutex };
  if (t_inSkyPass) {
    // Background drawn at infinite depth: not scene geometry for Remix (sky is handled separately).
    if (s_skyMeshes.insert(staticMesh).second) {
      log::line("sky pass mesh %p (ranges=%u) excluded", staticMesh,
                game::field<uint32_t>(staticMesh, game::kStaticMeshPrimitiveCount) & 0x3FFF);
    }
    return;
  }
  if (s_rejectedMeshes.count(staticMesh) || s_skyMeshes.count(staticMesh)) {
    return;
  }
  auto live = s_liveMeshes.find(staticMesh);
  if (live == s_liveMeshes.end()) {
    const uint32_t rangeCount = game::field<uint32_t>(staticMesh, game::kStaticMeshPrimitiveCount) & 0x3FFF;
    const std::vector<RangeState> culls = readRangeStates(meshInstance, rangeCount);
    Mesh mesh;
    mesh.key = (uint64_t(++s_generation) << 32) | reinterpret_cast<uintptr_t>(staticMesh);
    if (!decodeMesh(staticMesh, culls, decl, mesh)) {
      s_rejectedMeshes.insert(staticMesh);
      return;
    }
    if (masked) {
      CellTable table;
      if (readCellTable(item, staticMesh, table)) {
        splitByCells(table, mesh);
      } else {
        log::line("masked mesh %p: cell table not readable", staticMesh);
      }
    }
    live = s_liveMeshes.emplace(staticMesh, mesh.key).first;
    s_frame.newMeshes.push_back(std::move(mesh));
  }

  // The same placement is submitted once per render pass; keep one instance per frame.
  const uint64_t instanceKey = (uint64_t(reinterpret_cast<uintptr_t>(meshInstance)) << 32) |
                               reinterpret_cast<uintptr_t>(placement);
  if (s_frameInstances.insert(instanceKey).second) {
    Instance instance { live->second, game::field<game::Mat4>(placement, game::kPlacementWorld), {} };
    CellTable table;
    if (masked && readCellTable(item, staticMesh, table)) {
      for (uint32_t k = 0; k < table.count; ++k) {
        instance.cellVisible.push_back(cellVisible(table, k) ? 1 : 0);
      }
    }
    s_frame.instances.push_back(std::move(instance));
    const uint32_t channels =
        (game::field<uint32_t>(placement, game::kObjectFlags) >> game::kObjectChannelShift) & game::kObjectChannelMask;
    for (int bit = 0; bit < 8; ++bit) {
      s_frame.channelPlacements[bit] += (channels >> bit) & 1;
    }
  }
}

void __fastcall submitHook(void* meshInstance, void* /*edx*/, void* renderItem, void* matrixCtx, void* placement) {
  t_currentInstance = meshInstance;
  s_originalSubmit(meshInstance, renderItem, matrixCtx, placement);
  t_currentInstance = nullptr;
  recordInstance(meshInstance, placement, false, renderItem);
}

// Masked instances draw only the cells visible to the raster camera; the whole mesh is submitted to Remix.
void __fastcall maskedSubmitHook(void* meshInstance, void* /*edx*/, void* renderItem, void* matrixCtx,
                                 void* placement) {
  t_currentInstance = meshInstance;
  s_originalMaskedSubmit(meshInstance, renderItem, matrixCtx, placement);
  t_currentInstance = nullptr;
  recordInstance(meshInstance, placement, true, renderItem);
}

void __fastcall skyPassHook(void* pass, void* /*edx*/, void* list, void* a2, void* a3, void* a4, void* a5) {
  t_inSkyPass = true;
  s_originalSkyPass(pass, list, a2, a3, a4, a5);
  t_inSkyPass = false;
}

void __fastcall destructorHook(void* staticMesh, void* /*edx*/) {
  {
    std::lock_guard lock { s_mutex };
    s_skyMeshes.erase(staticMesh);
    s_rejectedMeshes.erase(staticMesh);
    auto live = s_liveMeshes.find(staticMesh);
    if (live != s_liveMeshes.end()) {
      s_frame.destroyedMeshes.push_back(live->second);
      s_liveMeshes.erase(live);
    }
  }
  s_originalDestructor(staticMesh);
}

} // namespace

bool install() {
  return hook::install("DX9StaticMeshInstance_Submit", game::kStaticMeshSubmit, game::kStaticMeshSubmitPrologue,
                       reinterpret_cast<void*>(&submitHook), &s_originalSubmit) &&
         hook::install("DX9MaskedStaticMeshInstance_Submit", game::kMaskedMeshSubmit,
                       game::kMaskedMeshSubmitPrologue, reinterpret_cast<void*>(&maskedSubmitHook),
                       &s_originalMaskedSubmit) &&
         hook::install("Renderer_SkyPass", game::kSkyPass, game::kSkyPassPrologue,
                       reinterpret_cast<void*>(&skyPassHook), &s_originalSkyPass) &&
         hook::install("DX9StaticMesh_Destructor", game::kStaticMeshDestructor, game::kStaticMeshDestructorPrologue,
                       reinterpret_cast<void*>(&destructorHook), &s_originalDestructor);
}

Frame takeFrame() {
  std::lock_guard lock { s_mutex };
  // Drop instances of sky meshes recorded by other passes before the sky pass identified them.
  std::unordered_set<uint64_t> skyKeys;
  for (const void* mesh : s_skyMeshes) {
    auto live = s_liveMeshes.find(mesh);
    if (live != s_liveMeshes.end()) {
      skyKeys.insert(live->second);
    }
  }
  if (!skyKeys.empty()) {
    auto& inst = s_frame.instances;
    inst.erase(std::remove_if(inst.begin(), inst.end(),
                              [&](const Instance& i) { return skyKeys.count(i.meshKey) != 0; }),
               inst.end());
  }
  Frame out = std::move(s_frame);
  s_frame = {};
  s_frameInstances.clear();
  return out;
}

uint8_t engineCull(const uint8_t* entry) {
  return cullOfEntry(entry);
}

uint64_t trackMesh(const void* staticMesh) {
  std::lock_guard lock { s_mutex };
  auto live = s_liveMeshes.find(staticMesh);
  if (live == s_liveMeshes.end()) {
    const uint64_t key = (uint64_t(++s_generation) << 32) | reinterpret_cast<uintptr_t>(staticMesh);
    live = s_liveMeshes.emplace(staticMesh, key).first;
  }
  return live->second;
}

const void* currentMeshInstance() {
  return t_currentInstance;
}

} // namespace ac1rtx::static_geometry
