#include "skinned.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "static_geometry.h"

#include <d3d9.h>

#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ac1rtx::skinned {

namespace {

using PaletteFn = void(__thiscall*)(void* entry, void* ctx, void* placement, void* palettes);
PaletteFn s_originalPalette = nullptr;

std::mutex s_mutex;
Frame s_frame;
std::set<std::tuple<uint64_t, uint32_t, uint8_t>> s_decoded;  // (mesh key, range, cull) already sent or rejected
std::unordered_map<const void*, bool> s_declValid;   // IDirect3DVertexDeclaration9* -> matches decoder
// (instance, placement, range, matInst): one draw per entry. The engine draws some ranges twice in the colour pass
// with different entries: CULL_CW with the outer material and CULL_CCW with the inner one (dx9 trace frame 0, e.g.
// seq 17584/17596: same 184 vertices / 242 triangles, different PS, VS and s0/s1 textures).
std::set<std::tuple<const void*, const void*, uint32_t, const void*>> s_frameDraws;
std::unordered_set<const void*> s_framePlacements;                        // placements counted this frame
// Every placement (render object) a skinned palette was ever built for: anticull keeps the engine LOD for them.
std::mutex s_knownMutex;
std::unordered_set<const void*> s_knownPlacements;
thread_local CurrentEntry t_current;

// The decoder implements exactly the layout the skinned vertex shaders consume (see game.h).
bool validateDeclaration(IDirect3DVertexDeclaration9* decl) {
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1] = {};
  UINT count = MAXD3DDECLLENGTH + 1;
  if (FAILED(decl->GetDeclaration(elements, &count))) {
    return false;
  }
  int position = -1, normal = -1, uv = -1, indices = -1, weights = -1;
  for (UINT i = 0; i < count && elements[i].Stream != 0xFF; ++i) {
    const auto& e = elements[i];
    if (e.Stream != 0 || e.UsageIndex != 0) {
      continue;
    }
    if (e.Usage == D3DDECLUSAGE_POSITION && e.Type == D3DDECLTYPE_SHORT4N) {
      position = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_NORMAL && e.Type == D3DDECLTYPE_UBYTE4) {
      normal = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.Type == D3DDECLTYPE_SHORT2N) {
      uv = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_BLENDINDICES && e.Type == D3DDECLTYPE_UBYTE4) {
      indices = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT && e.Type == D3DDECLTYPE_UBYTE4N) {
      weights = e.Offset;
    }
  }
  const bool valid = position == 0 && normal == 8 && uv == 20 && indices == 24 && weights == 28;
  log::line("skinned decl %p: pos@%d normal@%d uv@%d indices@%d weights@%d -> %s", decl, position, normal, uv, indices,
            weights, valid ? "accepted" : "REJECTED");
  return valid;
}

Vertex decodeVertex(const uint8_t* v) {
  Vertex out = {};
  const auto* p = reinterpret_cast<const int16_t*>(v);
  for (int i = 0; i < 3; ++i) {
    out.position[i] = p[i] * game::kSkinnedPositionScale;
    out.normal[i] = (v[8 + i] - game::kNormalBias) * game::kNormalScale;
  }
  const auto* uv = reinterpret_cast<const int16_t*>(v + 20);
  out.texcoord[0] = (uv[0] / 32767.0f) * game::kTexcoordScale;
  out.texcoord[1] = (uv[1] / 32767.0f) * game::kTexcoordScale;
  const uint32_t sum = uint32_t(v[28]) + v[29] + v[30] + v[31];
  for (int i = 0; i < 4; ++i) {
    out.bones[i] = v[24 + i];
    out.weights[i] = sum ? v[28 + i] / float(sum) : (i == 0 ? 1.0f : 0.0f);
  }
  return out;
}

template<typename Buffer, typename Desc>
const uint8_t* lockReadOnly(Buffer* buffer, Desc& desc) {
  void* data = nullptr;
  if (FAILED(buffer->GetDesc(&desc)) || FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) {
    return nullptr;
  }
  return static_cast<const uint8_t*>(data);
}

bool decodeRange(const void* staticMesh, uint32_t range, uint8_t cull, RangeMesh& out) {
  const uint32_t stride = game::field<uint32_t>(staticMesh, game::kStaticMeshStride) & 0x7FF;
  const void* vbWrapper = game::field<const void*>(staticMesh, game::kStaticMeshVertexBuffer);
  const void* ibWrapper = game::field<const void*>(staticMesh, game::kStaticMeshIndexBuffer);
  const uint32_t rangeCount = game::field<uint32_t>(staticMesh, game::kStaticMeshPrimitiveCount) & 0x3FFF;
  if (!vbWrapper || !ibWrapper || stride != game::kSkinnedStride || range >= rangeCount) {
    log::line("skinned mesh %p range %u: vb=%p ib=%p stride=%u ranges=%u - skipped", staticMesh, range, vbWrapper,
              ibWrapper, stride, rangeCount);
    return false;
  }
  const auto& r = game::field<const game::PrimitiveRange*>(staticMesh, game::kStaticMeshPrimitiveTable)[range];
  auto* vb = game::field<IDirect3DVertexBuffer9*>(vbWrapper, game::kDeviceBufferD3D);
  auto* ib = game::field<IDirect3DIndexBuffer9*>(ibWrapper, game::kDeviceBufferD3D);
  D3DVERTEXBUFFER_DESC vbDesc = {};
  D3DINDEXBUFFER_DESC ibDesc = {};
  const uint8_t* vertices = lockReadOnly(vb, vbDesc);
  if (!vertices) {
    return false;
  }
  const uint8_t* indices = lockReadOnly(ib, ibDesc);
  if (!indices) {
    vb->Unlock();
    return false;
  }
  const bool index32 = ibDesc.Format == D3DFMT_INDEX32;
  const uint32_t vertexCount = vbDesc.Size / stride;
  const uint32_t indexCount = ibDesc.Size / (index32 ? 4 : 2);
  auto indexAt = [&](uint32_t i) -> uint32_t {
    return index32 ? reinterpret_cast<const uint32_t*>(indices)[i] : reinterpret_cast<const uint16_t*>(indices)[i];
  };
  const uint32_t needed = r.type == D3DPT_TRIANGLELIST ? r.primitiveCount * 3 : r.primitiveCount + 2;
  bool ok = (r.type == D3DPT_TRIANGLELIST || r.type == D3DPT_TRIANGLESTRIP) && r.startIndex + needed <= indexCount &&
            r.minVertex + r.numVertices <= vertexCount;
  if (ok) {
    out.vertices.reserve(r.numVertices);
    for (uint32_t v = 0; v < r.numVertices; ++v) {
      out.vertices.push_back(decodeVertex(vertices + (r.minVertex + v) * stride));
    }
    // Same facing rule as static meshes: engine D3DCULL_CW ranges are reversed for Remix (static_geometry.cpp).
    const bool reverse = cull == 2;
    for (uint32_t t = 0; t < r.primitiveCount && ok; ++t) {
      uint32_t a, b, c;
      if (r.type == D3DPT_TRIANGLELIST) {
        a = indexAt(r.startIndex + t * 3), b = indexAt(r.startIndex + t * 3 + 1), c = indexAt(r.startIndex + t * 3 + 2);
      } else if (t & 1) {
        a = indexAt(r.startIndex + t + 1), b = indexAt(r.startIndex + t), c = indexAt(r.startIndex + t + 2);
      } else {
        a = indexAt(r.startIndex + t), b = indexAt(r.startIndex + t + 1), c = indexAt(r.startIndex + t + 2);
      }
      for (uint32_t idx : { a, b, c }) {
        ok = ok && idx >= r.minVertex && idx < r.minVertex + r.numVertices;
      }
      if (!ok || a == b || b == c || a == c) {
        continue;
      }
      if (reverse) {
        std::swap(b, c);
      }
      out.indices.insert(out.indices.end(), { a - r.minVertex, b - r.minVertex, c - r.minVertex });
    }
  }
  ib->Unlock();
  vb->Unlock();
  if (!ok) {
    log::line("skinned mesh %p range %u rejected (type=%u start=%u prims=%u min=%u num=%u)", staticMesh, range, r.type,
              r.startIndex, r.primitiveCount, r.minVertex, r.numVertices);
  }
  out.doubleSided = cull == 1;
  return ok && !out.indices.empty();
}

void __fastcall paletteHook(void* entry, void* /*edx*/, void* ctx, void* placement, void* palettes) {
  s_originalPalette(entry, ctx, placement, palettes);
  if (placement) {
    std::lock_guard known { s_knownMutex };
    s_knownPlacements.insert(placement);
  }
  if (!entry || !ctx || !palettes) {
    return;
  }
  const auto* e = static_cast<const uint8_t*>(entry);
  const void* instance = static_cast<const uint8_t*>(palettes) - game::kSkinnedPalettesInInstance;
  const void* staticMesh = game::field<const void*>(instance, game::kMeshInstanceStaticMesh);
  const uint32_t range = e[game::kDrawEntryRange] & 0x7F;
  t_current = { staticMesh, game::field<const void*>(e, game::kDrawEntryMaterialInstance), range };

  if (game::field<uint32_t>(ctx, game::kRenderItemPassType) > 1 || !staticMesh) {
    return;  // colour passes only: they use draw list 0 and bind the materials
  }
  const auto* records = game::field<const uint8_t*>(palettes, 0);
  const uint8_t* record = records ? records + e[game::kSkinnedEntryPalette] * game::kPaletteRecordSize : nullptr;
  const uint32_t boneCount = record ? game::field<uint32_t>(record, 0) : 0;
  const void* device = game::field<const void*>(ctx, game::kRenderItemDevice);
  if (boneCount == 0 || boneCount > game::kMaxBones || !device) {
    return;
  }

  std::lock_guard lock { s_mutex };
  const void* matInst = t_current.matInst;
  if (!s_frameDraws.emplace(instance, placement, range, matInst).second) {
    return;  // the same entry drawn again this frame with the same pose (second colour pass)
  }
  const uint8_t cull = static_geometry::engineCull(e);
  if (cull == 0 || cull == 4) {
    return;
  }
  if (placement && s_framePlacements.insert(placement).second) {
    const uint32_t channels =
        (game::field<uint32_t>(placement, game::kObjectFlags) >> game::kObjectChannelShift) & game::kObjectChannelMask;
    for (int bit = 0; bit < 8; ++bit) {
      s_frame.channelPlacements[bit] += (channels >> bit) & 1;
    }
  }
  const uint64_t meshKey = static_geometry::trackMesh(staticMesh);
  if (s_decoded.emplace(meshKey, range, cull).second) {
    auto* decl = game::field<IDirect3DVertexDeclaration9*>(instance, game::kMeshInstanceVertexDecl);
    auto declIt = decl ? s_declValid.find(decl) : s_declValid.end();
    if (decl && declIt == s_declValid.end()) {
      declIt = s_declValid.emplace(decl, validateDeclaration(decl)).first;
    }
    RangeMesh mesh;
    mesh.meshKey = meshKey;
    mesh.range = range;
    mesh.cull = cull;
    if (decl && declIt->second && decodeRange(staticMesh, range, cull, mesh)) {
      s_frame.newRanges.push_back(std::move(mesh));
    }
  }
  Draw draw;
  draw.meshKey = meshKey;
  draw.range = range;
  draw.matInst = matInst;
  draw.cull = cull;
  draw.bones.resize(boneCount * 12);
  for (uint32_t i = 0; i < boneCount; ++i) {
    for (int k = 0; k < 3; ++k) {
      const auto* reg = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) + game::kDeviceBoneShadow +
                                                       i * 64 + k * 16);
      for (int c = 0; c < 4; ++c) {
        draw.bones[i * 12 + k * 4 + c] = reg[c];
      }
    }
  }
  s_frame.draws.push_back(std::move(draw));
}

} // namespace

bool install() {
  return hook::install("SkinnedMesh_BuildPalette", game::kSkinnedPalette, game::kSkinnedPalettePrologue,
                       reinterpret_cast<void*>(&paletteHook), &s_originalPalette);
}

Frame takeFrame() {
  std::lock_guard lock { s_mutex };
  Frame out = std::move(s_frame);
  s_frame = {};
  s_frameDraws.clear();
  s_framePlacements.clear();
  return out;
}

void setLastDrawKey(const void* matInst, const void* materialKey) {
  std::lock_guard lock { s_mutex };
  if (!s_frame.draws.empty() && s_frame.draws.back().matInst == matInst) {
    s_frame.draws.back().matInst = materialKey;
  }
}

bool isSkinnedPlacement(const void* placement) {
  std::lock_guard known { s_knownMutex };
  return s_knownPlacements.count(placement) != 0;
}

bool takeCurrentEntry(const void* matInst, CurrentEntry& out) {
  if (!t_current.staticMesh || t_current.matInst != matInst) {
    return false;
  }
  out = t_current;
  t_current = {};
  return true;
}

} // namespace ac1rtx::skinned
