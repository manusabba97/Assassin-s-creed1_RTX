#include "dynamic_mesh.h"

#include "hook.h"
#include "log.h"

#include <d3d9.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ac1rtx::dynamic_mesh {

namespace {

using SubmitFn = void(__thiscall*)(void* instance, void* item, void* ctx, void* placement);
using DrawFn = void(__thiscall*)(void* resource, void* device, uint32_t primCount);
SubmitFn s_originalSubmit = nullptr;
DrawFn s_originalDraw = nullptr;

struct Current {
  const void* instance = nullptr;
  const void* item = nullptr;
};
thread_local Current t_current;
thread_local bool t_pending = false;
thread_local Draw t_pendingDraw;

std::mutex s_mutex;
Frame s_frame;
void __fastcall submitHook(void* instance, void* /*edx*/, void* item, void* ctx, void* placement) {
  const Current saved = t_current;
  t_current = { instance, item };
  s_originalSubmit(instance, item, ctx, placement);
  t_current = saved;
  t_pending = false;
}

// Vertex layout of a dynamic mesh declaration (stream 0 offsets, -1 when absent).
struct Layout {
  int position = -1, normal = -1, uv = -1, copy = -1;  // copy: TEXCOORD3 UBYTE4, the clutter copy index
};
std::unordered_map<const void*, Layout> s_layouts;  // IDirect3DVertexDeclaration9* -> layout
std::unordered_map<const void*, bool> s_rejectLogged;

Layout readLayout(IDirect3DVertexDeclaration9* decl) {
  Layout l;
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1] = {};
  UINT count = MAXD3DDECLLENGTH + 1;
  if (FAILED(decl->GetDeclaration(elements, &count))) {
    return l;
  }
  for (UINT i = 0; i < count && elements[i].Stream != 0xFF; ++i) {
    const auto& e = elements[i];
    if (e.Stream != 0) {
      continue;
    }
    if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3) {
      l.position = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3) {
      l.normal = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT2) {
      l.uv = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 3 && e.Type == D3DDECLTYPE_UBYTE4) {
      l.copy = e.Offset;
    }
  }
  log::line("dynamic decl %p: pos@%d normal@%d uv@%d copy@%d", decl, l.position, l.normal, l.uv, l.copy);
  return l;
}

Layout layoutOf(IDirect3DVertexDeclaration9* decl) {
  std::lock_guard lock { s_mutex };
  auto it = s_layouts.find(decl);
  if (it == s_layouts.end()) {
    it = s_layouts.emplace(decl, readLayout(decl)).first;
  }
  return it->second;
}

void logRejectOnce(const void* instance, const char* why, uint32_t a, uint32_t b) {
  std::lock_guard lock { s_mutex };
  if (s_rejectLogged.emplace(instance, true).second) {
    log::line("dynamic instance %p skipped: %s (%u, %u)", instance, why, a, b);
  }
}

uint64_t fnv(uint64_t h, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

// Runs right after the engine's DrawIndexedPrimitive; the entry's MaterialInstance_EndDraw follows the last draw of
// the entry (the instanced path draws an entry once per batch or per instance: only the first draw is read).
void capture(const void* resource, uint32_t primCount) {
  const void* instance = t_current.instance;
  const void* item = t_current.item;
  if (t_pending || !instance || !item || game::field<uint32_t>(item, game::kRenderItemPassType) > 1) {
    return;  // colour passes only
  }
  const uint32_t instanceCount = game::field<uint32_t>(instance, game::kDynamicBoneCount);
  const uint32_t type = game::field<uint16_t>(instance, game::kDynamicType) & 7;
  const bool clutterBatches = instanceCount != 0 && (type == 5 || type == 6);
  const auto* matrices = game::field<const float*>(instance, game::kDynamicMatrices);
  if (instanceCount != 0 && !matrices) {
    return;
  }
  auto* decl = game::field<IDirect3DVertexDeclaration9*>(instance, game::kDynamicDecl);
  const uint32_t stride = game::field<uint32_t>(resource, game::kDynStride);
  const uint32_t vertexCount = game::field<uint32_t>(resource, game::kDynVertexCount);
  const auto* cpu = game::field<const uint8_t*>(resource, game::kDynCpuVertices);
  const uint32_t cpuBytes = game::field<uint32_t>(resource, game::kDynCpuBytes);
  const uint32_t primType = game::field<uint32_t>(resource, game::kDynPrimType);
  // Instanced batches: the copy-0 triangles lie within the first draw's primitives (R+0x28 * n / 32, n >= 1).
  primCount = std::min(primCount, game::field<uint32_t>(resource, game::kDynMaxPrims));
  const uint32_t slot = game::field<uint32_t>(resource, game::kDynIndexBufferSlot);
  const void* ibWrapper = slot < 4 ? game::field<const void*>(resource, game::kDynIndexBuffers + slot * 4) : nullptr;
  auto* ib = ibWrapper ? game::field<IDirect3DIndexBuffer9*>(ibWrapper, game::kDeviceBufferD3D) : nullptr;
  if (!decl || !cpu || !ib || vertexCount == 0 || size_t(vertexCount) * stride > cpuBytes ||
      (primType != D3DPT_TRIANGLELIST && primType != D3DPT_TRIANGLESTRIP)) {
    return;
  }
  const Layout layout = layoutOf(decl);
  if (instanceCount == 0) {
    if (stride != 32 || layout.position != 0 || layout.normal != 12 || layout.uv != 24) {
      return;  // cloth layout (frame_capture_01 seq 17825)
    }
  } else if (layout.position < 0 || layout.normal < 0 || layout.uv < 0 || (clutterBatches && layout.copy < 0)) {
    logRejectOnce(instance, "vertex layout", type, stride);
    return;
  }

  Draw draw;
  draw.resource = resource;
  // Instanced batches: the VB holds kClutterBatch copies of the mesh; only copy 0 is kept.
  if (clutterBatches) {
    uint32_t maxCopy = 0;
    for (uint32_t v = 0; v < vertexCount; ++v) {
      maxCopy = std::max<uint32_t>(maxCopy, cpu[size_t(v) * stride + layout.copy]);
    }
    if (maxCopy + 1 != game::kClutterBatch) {
      logRejectOnce(instance, "copies per batch", maxCopy + 1, instanceCount);  // 128-instance terrain batches
      return;
    }
  }
  std::vector<uint32_t> remap(vertexCount, UINT32_MAX);
  for (uint32_t v = 0; v < vertexCount; ++v) {
    const uint8_t* src = cpu + size_t(v) * stride;
    if (clutterBatches && src[layout.copy] != 0) {
      continue;
    }
    remap[v] = static_cast<uint32_t>(draw.vertices.size());
    Draw::Vertex out;
    std::memcpy(out.position, src + layout.position, sizeof(out.position));
    std::memcpy(out.normal, src + layout.normal, sizeof(out.normal));
    std::memcpy(out.texcoord, src + layout.uv, sizeof(out.texcoord));
    draw.vertices.push_back(out);
  }

  D3DINDEXBUFFER_DESC ibDesc = {};
  void* data = nullptr;
  if (FAILED(ib->GetDesc(&ibDesc)) || FAILED(ib->Lock(0, 0, &data, D3DLOCK_READONLY))) {
    return;
  }
  const bool index32 = ibDesc.Format == D3DFMT_INDEX32;
  const uint32_t indexCount = ibDesc.Size / (index32 ? 4 : 2);
  auto indexAt = [&](uint32_t i) -> uint32_t {
    return index32 ? static_cast<const uint32_t*>(data)[i] : static_cast<const uint16_t*>(data)[i];
  };
  const uint32_t needed = primType == D3DPT_TRIANGLELIST ? primCount * 3 : primCount + 2;
  if (needed <= indexCount) {
    for (uint32_t t = 0; t < primCount; ++t) {
      uint32_t a, b, c;
      if (primType == D3DPT_TRIANGLELIST) {
        a = indexAt(t * 3), b = indexAt(t * 3 + 1), c = indexAt(t * 3 + 2);
      } else if (t & 1) {
        a = indexAt(t + 1), b = indexAt(t), c = indexAt(t + 2);
      } else {
        a = indexAt(t), b = indexAt(t + 1), c = indexAt(t + 2);
      }
      if (a < vertexCount && b < vertexCount && c < vertexCount && a != b && b != c && a != c &&
          remap[a] != UINT32_MAX && remap[b] != UINT32_MAX && remap[c] != UINT32_MAX) {
        draw.indices.insert(draw.indices.end(), { remap[a], remap[b], remap[c] });
      }
    }
  }
  ib->Unlock();
  if (draw.indices.empty()) {
    return;
  }

  if (instanceCount == 0) {
    draw.world = game::field<game::Mat4>(item, game::kItemWorld);
  } else {
    draw.instances.resize(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
      const float* m = matrices + size_t(i) * 16;
      auto& t = draw.instances[i];
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          // Batches: raw registers (row r = output r). Per-instance draws: a row-vector world, as g_World.
          t.m[r][c] = clutterBatches ? m[r * 4 + c] : m[c * 4 + r];
        }
      }
    }
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, draw.vertices.data(), draw.vertices.size() * sizeof(Draw::Vertex));
    h = fnv(h, draw.indices.data(), draw.indices.size() * sizeof(uint32_t));
    draw.geometryHash = h;
  }
  t_pendingDraw = std::move(draw);
  t_pending = true;
}

void __fastcall drawHook(void* resource, void* /*edx*/, void* device, uint32_t primCount) {
  s_originalDraw(resource, device, primCount);
  if (resource) {
    capture(resource, primCount);
  }
}

// Cull of the entry that uses matInst (FUN_00A4A580 via FUN_00A4B1B0 with the dynamic entry bits):
// forced winding (matInst +0xC bit0 or entry bit0) -> CW / CCW by entry bit0; else material bit25 NONE, bit19 CCW.
// Returns 0 when the entry is not found or is drawn without depth write (not submitted, as for static meshes).
uint8_t entryCull(const void* instance, const void* matInst) {
  const auto* entries = game::field<const uint8_t*>(instance, game::kDynamicEntries);
  const uint32_t count = game::field<uint32_t>(instance, game::kDynamicEntryCount) & 0x3FFF;
  for (uint32_t e = 0; entries && e < count; ++e) {
    const uint8_t* entry = entries + e * game::kDynamicEntrySize;
    if (game::field<const void*>(entry, 0) != matInst) {
      continue;
    }
    const void* material = game::field<const void*>(matInst, game::kMaterialInstanceMaterial);
    if (!material) {
      return 0;
    }
    const uint32_t flags = game::field<uint32_t>(material, game::kMaterialFlags);
    const uint8_t bits = entry[game::kDynamicEntryFlags];
    const uint32_t noDepthWriteBit = (flags & game::kMaterialBlendMask) == 0 ? game::kMaterialNoDepthWriteOpaque
                                                                              : game::kMaterialNoDepthWriteBlended;
    const bool depthWrite = (bits & 2) ? true : (bits & 4) ? false : (flags & noDepthWriteBit) == 0;
    if (!depthWrite || (flags & game::kMaterialHidden)) {
      return 0;
    }
    const bool instanceForced = (game::field<uint8_t>(matInst, game::kMaterialInstanceFlags) & 1) != 0;
    if (instanceForced || (bits & 1)) {
      return (bits & 1) ? 3 : 2;
    }
    if (flags & game::kMaterialNoCull) {
      return 1;
    }
    return (flags & game::kMaterialCullCCW) ? 3 : 2;
  }
  return 0;
}

} // namespace

bool install() {
  return hook::install("DX9DynamicSubMeshInstance_Submit", game::kDynamicSubmit, game::kDynamicSubmitPrologue,
                       reinterpret_cast<void*>(&submitHook), &s_originalSubmit) &&
         hook::install("DynamicMesh_Draw", game::kDynamicDraw, game::kDynamicDrawPrologue,
                       reinterpret_cast<void*>(&drawHook), &s_originalDraw);
}

const void* onEndDraw(const void* matInst) {
  if (!t_pending || !t_current.instance) {
    return nullptr;
  }
  t_pending = false;
  Draw draw = std::move(t_pendingDraw);
  const uint8_t cull = entryCull(t_current.instance, matInst);
  if (cull == 0) {
    return nullptr;
  }
  // Same facing rule as the other engine meshes (static_geometry.cpp decodeMesh): CW reversed, CCW kept, NONE
  // double-sided.
  if (cull == 2) {
    for (size_t i = 0; i + 2 < draw.indices.size(); i += 3) {
      std::swap(draw.indices[i + 1], draw.indices[i + 2]);
    }
  }
  draw.doubleSided = cull == 1;
  draw.matInst = matInst;
  const void* resource = draw.resource;
  std::lock_guard lock { s_mutex };
  s_frame.draws.push_back(std::move(draw));
  return resource;
}

Frame takeFrame() {
  std::lock_guard lock { s_mutex };
  Frame out = std::move(s_frame);
  s_frame = {};
  return out;
}

} // namespace ac1rtx::dynamic_mesh
