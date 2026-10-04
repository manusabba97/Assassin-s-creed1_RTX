#include "remix.h"

#include "camera.h"
#include "dynamic_mesh.h"
#include "lights.h"
#include "log.h"
#include "materials.h"
#include "pbr.h"
#include "pbr_edit.h"
#include "skinned.h"
#include "specular_table.h"
#include "static_geometry.h"
#include "foliage_shared.h"

#include <d3d9.h>

#include <remixapi/bridge_remix_api.h>

#include <algorithm>
#include <cstdarg>
#include <random>
#include <unordered_set>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ac1rtx::remix {

namespace {

constexpr uint64_t kStaticMaterialHash = 0xAC1000000000B001ull;

Settings s_settings;
bool s_initialized = false;
bool s_failed = false;
remixapi_Interface s_api = {};
remixapi_MaterialHandle s_staticMaterial = nullptr;
// remixapi_InstanceInfo::doubleSided is per instance, so each engine mesh becomes one Remix mesh per sidedness;
// masked meshes are further split per engine cell, so their visible cells can be drawn individually.
struct StaticMeshPart {
  remixapi_MeshHandle handle = nullptr;
  bool doubleSided = false;
  uint32_t cell = static_geometry::Surface::kNoCell;
  uint8_t blend = 0;   // engine blend mode of ranges drawn without depth write (game.h kBlend*), 0 = opaque
  bool decal = false;  // drawn by the decal pass (render list 1)
  uint8_t layer = 0;   // terrain layer overlay (1/2) or 0
  bool perRange = false;                    // single range with layer weights (static_geometry Surface::perRange)
  uint32_t range = 0;                       // that range when perRange
  bool vertexAlphaOne = true;               // every vertex colour alpha of the part is 255
  // Blended single-range parts keep their geometry: water is re-meshed every frame with animated texcoords.
  std::shared_ptr<const std::vector<remixapi_HardcodedVertex>> vertices;
  std::shared_ptr<const std::vector<uint32_t>> indices;
};
constexpr uint64_t kDoubleSidedHashSalt = 0xD5D5000000000000ull;
std::unordered_map<uint64_t, std::vector<StaticMeshPart>> s_staticMeshes;

struct Vec3 {
  float x, y, z;
};

// Per-frame cost measurement: DrawInstance calls (each one crosses the bridge) and CPU time spent in onPresent.
uint32_t s_frameInstances = 0;
uint64_t s_materialsCreated = 0;  // CreateMaterial calls (perf log: re-creation churn)
void drawInstance(const remixapi_InstanceInfo* info) {
  ++s_frameInstances;
  s_api.DrawInstance(info);
}

// Neutral grey fallback for surfaces whose material could not be created.
void createStaticMaterial() {
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = { 0.5f, 0.5f, 0.5f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 0.7f;
  opaque.thinFilmThickness_value = 200.0f;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo material = {};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = kStaticMaterialHash;
  material.spriteSheetRow = 1;
  material.spriteSheetCol = 1;
  material.filterMode = 1;
  material.wrapModeU = 1;
  material.wrapModeV = 1;
  s_api.CreateMaterial(&material, &s_staticMaterial);
}

// Mesh keys are (generation << 32) | DX9StaticMesh* (static_geometry.cpp).
const void* staticMeshOf(uint64_t meshKey) {
  return reinterpret_cast<const void*>(static_cast<uintptr_t>(meshKey & 0xFFFFFFFFull));
}

// ---- Textures: engine IDirect3DTexture9 -> Remix texture (pixels read from the bridge client's level shadows).

struct RemixTexture {
  uint64_t hash = 0;
  bool valid = false;
  remixapi_TextureHandle handle = nullptr;
  uint64_t contentHash = 0;  // FNV-1a 64 of the level-0 bytes (+ size, format): stable across sessions (pbr.h)
};
std::unordered_map<IDirect3DTexture9*, RemixTexture> s_textures;  // key pinned with AddRef
uint32_t s_textureSerial = 0;

bool isBlockCompressed(D3DFORMAT f) {
  return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5;
}

// Remix format for a D3D9 format, plus how its pixels are copied (see d3d9_format.cpp swizzles in dxvk-remix).
bool remixFormatFor(D3DFORMAT f, remixapi_Format& out) {
  switch (f) {
    case D3DFMT_DXT1: out = REMIXAPI_FORMAT_BC1_RGBA_UNORM; return true;
    case D3DFMT_DXT3: out = REMIXAPI_FORMAT_BC2_UNORM; return true;
    case D3DFMT_DXT5: out = REMIXAPI_FORMAT_BC3_UNORM; return true;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A8L8: out = REMIXAPI_FORMAT_B8G8R8A8_UNORM; return true;
    default: return false;
  }
}

// Appends one mip level, tightly packed, converting 8/16-bit formats to BGRA8.
bool appendLevel(IDirect3DTexture9* tex, UINT level, D3DFORMAT format, std::vector<uint8_t>& out) {
  D3DSURFACE_DESC desc = {};
  if (FAILED(tex->GetLevelDesc(level, &desc))) {
    return false;
  }
  D3DLOCKED_RECT locked = {};
  if (FAILED(tex->LockRect(level, &locked, nullptr, D3DLOCK_READONLY))) {
    return false;
  }
  const auto* src = static_cast<const uint8_t*>(locked.pBits);
  if (isBlockCompressed(format)) {
    const uint32_t blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
    const uint32_t rowBytes = ((desc.Width + 3) / 4) * blockBytes;
    const uint32_t rows = (desc.Height + 3) / 4;
    for (uint32_t y = 0; y < rows; ++y) {
      out.insert(out.end(), src + y * locked.Pitch, src + y * locked.Pitch + rowBytes);
    }
  } else {
    for (uint32_t y = 0; y < desc.Height; ++y) {
      const uint8_t* row = src + y * locked.Pitch;
      for (uint32_t x = 0; x < desc.Width; ++x) {
        uint8_t b, g, r, a;
        switch (format) {
          case D3DFMT_A8R8G8B8: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = row[x * 4 + 3]; break;
          case D3DFMT_X8R8G8B8: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = 0xFF; break;
          case D3DFMT_A8: b = g = r = 0; a = row[x]; break;                          // swizzle ZERO,ZERO,ZERO,R
          case D3DFMT_L8: b = g = r = row[x]; a = 0xFF; break;                       // swizzle R,R,R,ONE
          default /* A8L8 */: b = g = r = row[x * 2]; a = row[x * 2 + 1]; break;     // swizzle R,R,R,G
        }
        out.insert(out.end(), { b, g, r, a });
      }
    }
  }
  tex->UnlockRect(level);
  return true;
}

std::wstring texturePath(uint64_t hash) {
  wchar_t buf[24];
  swprintf_s(buf, L"0x%016llX", static_cast<unsigned long long>(hash));
  return buf;
}

// Uploads `tex` on first use; returns its hash (0 if it cannot be converted).
uint64_t ensureTexture(IDirect3DTexture9* tex) {
  auto it = s_textures.find(tex);
  if (it != s_textures.end()) {
    return it->second.valid ? it->second.hash : 0;
  }
  tex->AddRef();
  RemixTexture entry;
  D3DSURFACE_DESC desc = {};
  remixapi_Format format = {};
  if (SUCCEEDED(tex->GetLevelDesc(0, &desc)) && remixFormatFor(desc.Format, format)) {
    const UINT levels = tex->GetLevelCount();
    std::vector<uint8_t> data;
    bool ok = true;
    size_t level0Size = 0;
    for (UINT l = 0; l < levels && ok; ++l) {
      ok = appendLevel(tex, l, desc.Format, data);
      if (l == 0) {
        level0Size = data.size();
      }
    }
    if (ok) {
      const uint32_t header[3] = { desc.Width, desc.Height, static_cast<uint32_t>(desc.Format) };
      entry.contentHash = pbr::fnv1a(data.data(), level0Size,
                                     pbr::fnv1a(reinterpret_cast<const uint8_t*>(header), sizeof(header)));
      entry.hash = 0xAC1E000000000000ull | (uint64_t(++s_textureSerial) << 32) | reinterpret_cast<uintptr_t>(tex);
      remixapi_TextureInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      info.hash = entry.hash;
      info.width = desc.Width;
      info.height = desc.Height;
      info.depth = 1;
      info.mipLevels = levels;
      info.format = format;
      info.data = data.data();
      info.dataSize = data.size();
      const remixapi_ErrorCode err = s_api.CreateTexture(&info, &entry.handle);
      entry.valid = err == REMIXAPI_ERROR_CODE_SUCCESS;
      if (!entry.valid) {
        log::line("CreateTexture %p fmt=%u %ux%u mips=%u bytes=%zu -> %d", tex, desc.Format, desc.Width, desc.Height,
                  levels, data.size(), err);
      }
    }
  } else {
    log::line("texture %p format %u not converted", tex, desc.Format);
  }
  s_textures.emplace(tex, entry);
  return entry.valid ? entry.hash : 0;
}

// ---- Normal maps: engine tangent-space RGB (see game.h) -> Remix unsigned octahedral in BC5.
// Engine decode: n = (2r-1)T + (2g-1)B + (2b-1)N with B opposite to dP/dv; Remix uses T = dP/du and B along dP/dv
// (genTangSpace, math.slangh:415), so green flips. Remix encoding (packing.slangh:315-361):
// p = n.xy / (|x| + |y| + z), enc = (p.x + p.y, p.x - p.y) * 0.5 + 0.5.

std::unordered_map<IDirect3DTexture9*, RemixTexture> s_normalTextures;  // key pinned with AddRef

// Decodes the colour part of a BC1/BC3 block to 16 RGB texels (row-major).
void decodeColorBlock(const uint8_t* block, bool bc1, uint8_t rgb[16][3]) {
  const uint16_t c0 = block[0] | (block[1] << 8);
  const uint16_t c1 = block[2] | (block[3] << 8);
  uint8_t palette[4][3];
  auto expand = [](uint16_t c, uint8_t out[3]) {
    out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  for (int i = 0; i < 3; ++i) {
    if (!bc1 || c0 > c1) {
      palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
    } else {
      palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
  }
  const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
  for (int t = 0; t < 16; ++t) {
    const uint32_t idx = (bits >> (t * 2)) & 3;
    for (int i = 0; i < 3; ++i) {
      rgb[t][i] = palette[idx][i];
    }
  }
}

// BC4 block (one channel of BC5) for 16 values: 8-value mode (e0 > e1), nearest palette entry per texel.
void encodeBC4(const uint8_t v[16], uint8_t out[8]) {
  uint8_t lo = 255, hi = 0;
  for (int t = 0; t < 16; ++t) {
    lo = v[t] < lo ? v[t] : lo;
    hi = v[t] > hi ? v[t] : hi;
  }
  out[0] = hi;
  out[1] = lo;
  uint64_t indices = 0;
  if (hi != lo) {
    int palette[8] = { hi, lo };
    for (int i = 1; i <= 6; ++i) {
      palette[i + 1] = ((7 - i) * hi + i * lo) / 7;
    }
    for (int t = 0; t < 16; ++t) {
      int best = 0, bestErr = 1 << 30;
      for (int p = 0; p < 8; ++p) {
        const int err = std::abs(palette[p] - v[t]);
        if (err < bestErr) {
          bestErr = err;
          best = p;
        }
      }
      indices |= uint64_t(best) << (t * 3);
    }
  }
  for (int i = 0; i < 6; ++i) {
    out[2 + i] = static_cast<uint8_t>(indices >> (i * 8));
  }
}

void encodeNormalTexel(const uint8_t rgb[3], uint8_t& ox, uint8_t& oy) {
  float n[3] = { rgb[0] / 127.5f - 1.0f, -(rgb[1] / 127.5f - 1.0f), rgb[2] / 127.5f - 1.0f };
  n[2] = n[2] < 0.0f ? 0.0f : n[2];  // Remix stores a hemisphere
  const float sum = std::fabs(n[0]) + std::fabs(n[1]) + n[2];
  float px = 0.0f, py = 0.0f;
  if (sum > 0.0f) {
    px = n[0] / sum;
    py = n[1] / sum;
  }
  const float ex = (px + py) * 0.5f + 0.5f;
  const float ey = (px - py) * 0.5f + 0.5f;
  ox = static_cast<uint8_t>(std::lround(std::clamp(ex, 0.0f, 1.0f) * 255.0f));
  oy = static_cast<uint8_t>(std::lround(std::clamp(ey, 0.0f, 1.0f) * 255.0f));
}

// Converts one mip level (tightly packed source from appendLevel) to BC5 blocks.
void convertNormalLevel(const std::vector<uint8_t>& src, uint32_t width, uint32_t height, D3DFORMAT format,
                        std::vector<uint8_t>& out) {
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint8_t rgb[16][3];
      if (format == D3DFMT_DXT1 || format == D3DFMT_DXT5) {
        const uint32_t blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
        const uint8_t* block = src.data() + (by * bw + bx) * blockBytes + (format == D3DFMT_DXT5 ? 8 : 0);
        decodeColorBlock(block, format == D3DFMT_DXT1, rgb);
      } else {  // BGRA8 from appendLevel
        for (int t = 0; t < 16; ++t) {
          const uint32_t x = std::min(bx * 4 + (t & 3), width - 1), y = std::min(by * 4 + (t >> 2), height - 1);
          const uint8_t* p = src.data() + (y * width + x) * 4;
          rgb[t][0] = p[2];
          rgb[t][1] = p[1];
          rgb[t][2] = p[0];
        }
      }
      uint8_t xs[16], ys[16];
      for (int t = 0; t < 16; ++t) {
        encodeNormalTexel(rgb[t], xs[t], ys[t]);
      }
      uint8_t block[16];
      encodeBC4(xs, block);
      encodeBC4(ys, block + 8);
      out.insert(out.end(), block, block + 16);
    }
  }
}

uint64_t ensureNormalTexture(IDirect3DTexture9* tex) {
  auto it = s_normalTextures.find(tex);
  if (it != s_normalTextures.end()) {
    return it->second.valid ? it->second.hash : 0;
  }
  tex->AddRef();
  RemixTexture entry;
  D3DSURFACE_DESC desc = {};
  remixapi_Format ignored = {};
  if (SUCCEEDED(tex->GetLevelDesc(0, &desc)) && remixFormatFor(desc.Format, ignored) &&
      desc.Format != D3DFMT_DXT3) {
    const UINT levels = tex->GetLevelCount();
    std::vector<uint8_t> data;
    bool ok = true;
    for (UINT l = 0; l < levels && ok; ++l) {
      D3DSURFACE_DESC level = {};
      std::vector<uint8_t> src;
      ok = SUCCEEDED(tex->GetLevelDesc(l, &level)) && appendLevel(tex, l, desc.Format, src);
      if (ok) {
        convertNormalLevel(src, level.Width, level.Height, desc.Format, data);
      }
    }
    if (ok) {
      entry.hash = 0xAC1F000000000000ull | (uint64_t(++s_textureSerial) << 32) | reinterpret_cast<uintptr_t>(tex);
      remixapi_TextureInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      info.hash = entry.hash;
      info.width = desc.Width;
      info.height = desc.Height;
      info.depth = 1;
      info.mipLevels = levels;
      info.format = REMIXAPI_FORMAT_BC5_UNORM;
      info.data = data.data();
      info.dataSize = data.size();
      const remixapi_ErrorCode err = s_api.CreateTexture(&info, &entry.handle);
      entry.valid = err == REMIXAPI_ERROR_CODE_SUCCESS;
      if (!entry.valid) {
        log::line("CreateTexture(normal) %p fmt=%u %ux%u -> %d", tex, desc.Format, desc.Width, desc.Height, err);
      }
    }
  } else {
    log::line("normal map %p format %u not converted", tex, desc.Format);
  }
  s_normalTextures.emplace(tex, entry);
  return entry.valid ? entry.hash : 0;
}

// ---- Skin albedo (game.h kSkinPs): the PS diffuse lerp(tex(s1), c128, tex(s2).x) * saturate(tex(s3) + c129) is
// composed on the CPU at every mip level of the base texture (mask and modulate sampled from their level of the
// closest size, nearest texel; an unbound sampler reads 0 in D3D9). Output alpha is 1 (the PS writes COLOR0.x).

// One mip level decoded to tightly packed RGB8.
// Alpha of one 4x4 block: DXT1 punch-through (index 3 when c0 <= c1), DXT3 explicit 4 bits, DXT5 interpolated
// (two endpoints, 3-bit indices; 8-value mode when a0 > a1, else 6 values plus 0 and 255).
void decodeAlphaBlock(const uint8_t* block, D3DFORMAT format, uint8_t alpha[16]) {
  if (format == D3DFMT_DXT1) {
    const uint16_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
    const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
    for (int t = 0; t < 16; ++t) alpha[t] = (c0 <= c1 && ((bits >> (t * 2)) & 3) == 3) ? 0 : 255;
  } else if (format == D3DFMT_DXT3) {
    for (int t = 0; t < 16; ++t) alpha[t] = static_cast<uint8_t>(((block[t / 2] >> ((t & 1) * 4)) & 15) * 17);
  } else {
    const int a0 = block[0], a1 = block[1];
    int palette[8] = { a0, a1 };
    if (a0 > a1) {
      for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
      for (int i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
      palette[6] = 0;
      palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
    for (int t = 0; t < 16; ++t) alpha[t] = static_cast<uint8_t>(palette[(bits >> (t * 3)) & 7]);
  }
}

// Level `level` of `tex` as RGB (and its alpha when `alpha` is given).
bool readLevelRgb(IDirect3DTexture9* tex, UINT level, std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h,
                  std::vector<uint8_t>* alpha = nullptr) {
  D3DSURFACE_DESC top = {}, desc = {};
  remixapi_Format ignored = {};
  std::vector<uint8_t> src;
  if (FAILED(tex->GetLevelDesc(0, &top)) || !remixFormatFor(top.Format, ignored) ||
      FAILED(tex->GetLevelDesc(level, &desc)) || !appendLevel(tex, level, top.Format, src)) {
    return false;
  }
  w = desc.Width;
  h = desc.Height;
  rgb.assign(size_t(w) * h * 3, 0);
  if (alpha) {
    alpha->assign(size_t(w) * h, 255);
  }
  if (isBlockCompressed(top.Format)) {
    const uint32_t blockBytes = top.Format == D3DFMT_DXT1 ? 8 : 16;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t block[16][3];
        const uint8_t* b = src.data() + (size_t(by) * bw + bx) * blockBytes;
        decodeColorBlock(b + (blockBytes == 16 ? 8 : 0), top.Format == D3DFMT_DXT1, block);
        uint8_t a[16];
        if (alpha) {
          decodeAlphaBlock(b, top.Format, a);
        }
        for (int t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
          if (x < w && y < h) {
            std::memcpy(&rgb[(size_t(y) * w + x) * 3], block[t], 3);
            if (alpha) {
              (*alpha)[size_t(y) * w + x] = a[t];
            }
          }
        }
      }
    }
  } else {  // BGRA8 from appendLevel
    for (size_t i = 0; i < size_t(w) * h; ++i) {
      rgb[i * 3] = src[i * 4 + 2];
      rgb[i * 3 + 1] = src[i * 4 + 1];
      rgb[i * 3 + 2] = src[i * 4];
      if (alpha) {
        (*alpha)[i] = src[i * 4 + 3];
      }
    }
  }
  return true;
}

// Level of `tex` closest in size to `width`: the smallest level still at least that wide (level 0 otherwise).
UINT matchingLevel(IDirect3DTexture9* tex, uint32_t width) {
  for (UINT l = tex->GetLevelCount(); l-- > 0;) {
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(tex->GetLevelDesc(l, &desc)) && desc.Width >= width) {
      return l;
    }
  }
  return 0;
}

using SkinKey = std::tuple<IDirect3DTexture9*, IDirect3DTexture9*, IDirect3DTexture9*, float, float, float, float,
                           float, float>;  // (base, mask, modulate, tint rgb, bias rgb)
std::map<SkinKey, RemixTexture> s_skinTextures;
std::unordered_map<IDirect3DTexture9*, uint32_t> s_skinPins;  // source texture -> AddRefs held by skin composites

uint64_t ensureSkinTexture(const materials::SurfaceMaterial& m) {
  const SkinKey key { m.albedo, m.skinMask, m.skinModulate, m.skinTint[0], m.skinTint[1], m.skinTint[2],
                      m.skinBias[0], m.skinBias[1], m.skinBias[2] };
  auto it = s_skinTextures.find(key);
  if (it != s_skinTextures.end()) {
    return it->second.valid ? it->second.hash : 0;
  }
  RemixTexture entry;
  D3DSURFACE_DESC desc = {};
  bool ok = SUCCEEDED(m.albedo->GetLevelDesc(0, &desc));
  const UINT levels = ok ? m.albedo->GetLevelCount() : 0;
  std::vector<uint8_t> data;
  for (UINT l = 0; l < levels && ok; ++l) {
    std::vector<uint8_t> base, mask, modulate;
    uint32_t w = 0, h = 0, mw = 0, mh = 0, dw = 0, dh = 0;
    ok = readLevelRgb(m.albedo, l, base, w, h);
    if (ok && m.skinMask) {
      ok = readLevelRgb(m.skinMask, matchingLevel(m.skinMask, w), mask, mw, mh);
    }
    if (ok && m.skinModulate) {
      ok = readLevelRgb(m.skinModulate, matchingLevel(m.skinModulate, w), modulate, dw, dh);
    }
    if (!ok) {
      break;
    }
    const size_t offset = data.size();
    data.resize(offset + size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* b = &base[(size_t(y) * w + x) * 3];
        const float k = mask.empty() ? 0.0f : mask[(size_t(y * mh / h) * mw + x * mw / w) * 3] / 255.0f;
        const uint8_t* d = modulate.empty() ? nullptr : &modulate[(size_t(y * dh / h) * dw + x * dw / w) * 3];
        uint8_t* out = &data[offset + (size_t(y) * w + x) * 4];
        for (int c = 0; c < 3; ++c) {
          const float lerped = b[c] / 255.0f + (m.skinTint[c] - b[c] / 255.0f) * k;
          const float mod = std::clamp((d ? d[c] / 255.0f : 0.0f) + m.skinBias[c], 0.0f, 1.0f);
          out[2 - c] = static_cast<uint8_t>(std::lround(std::clamp(lerped * mod, 0.0f, 1.0f) * 255.0f));
        }
        out[3] = 255;
      }
    }
  }
  if (ok) {
    entry.hash = 0xAC1C000000000000ull | (uint64_t(++s_textureSerial) << 32) | reinterpret_cast<uintptr_t>(m.albedo);
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = entry.hash;
    info.width = desc.Width;
    info.height = desc.Height;
    info.depth = 1;
    info.mipLevels = levels;
    info.format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
    info.data = data.data();
    info.dataSize = data.size();
    const remixapi_ErrorCode err = s_api.CreateTexture(&info, &entry.handle);
    entry.valid = err == REMIXAPI_ERROR_CODE_SUCCESS;
    if (!entry.valid) {
      log::line("skin texture %p mask %p modulate %p tint %.3f %.3f %.3f bias %.3f %.3f %.3f %ux%u -> %d", m.albedo,
                m.skinMask, m.skinModulate, m.skinTint[0], m.skinTint[1], m.skinTint[2], m.skinBias[0],
                m.skinBias[1], m.skinBias[2], desc.Width, desc.Height, err);
    }
  } else {
    log::line("skin texture %p not composed (mask %p modulate %p)", m.albedo, m.skinMask, m.skinModulate);
  }
  for (IDirect3DTexture9* tex : { m.albedo, m.skinMask, m.skinModulate }) {
    if (tex) {
      tex->AddRef();
      ++s_skinPins[tex];
    }
  }
  s_skinTextures.emplace(key, entry);
  return entry.valid ? entry.hash : 0;
}

uint32_t skinPins(IDirect3DTexture9* tex) {
  const auto it = s_skinPins.find(tex);
  return it != s_skinPins.end() ? it->second : 0;
}

// ---- PBR overrides (pbr.h): maps made offline for an engine albedo, found by the albedo's content hash. They replace
// the albedo, the engine normal map and the specular-derived roughness, and add metallic and height (POM).

struct PbrTextures {
  bool valid = false;           // maps uploaded and the material enabled in the editor
  uint64_t albedo = 0, normal = 0, roughness = 0, metallic = 0, height = 0;
  float heightMetres = 0.0f;
  float reliefUv = 0.0f;        // relief per texture repeat (tools/pbr_pack.py tune), 0 = heightMetres
  bool pomDefault = true;       // POM on unless the editor says otherwise
  float metallicConstant = 0.0f;  // metal class without a metallic map
  pbr::Edit edit;               // hand edits (material editor, key M in the Remix runtime)
  uint64_t editStamp = 0;       // last write time of the edit file when applied
  bool pending = false;         // a rebuild is running on the worker thread
};
std::unordered_map<uint64_t, PbrTextures> s_pbrTextures;  // albedo content hash -> uploaded maps (session lifetime)
std::unordered_map<uint64_t, remixapi_TextureHandle> s_pbrHandles;  // PBR texture hash -> handle
std::set<uint64_t> s_pbrDumped;

enum class PbrMap { kColor, kNormal, kGray, kHeight };

// One RGBA8 mip level -> the next (2x2 box; kHeight keeps the maximum, since QuadtreePOM "relies on special mipmaps
// with maximum values instead of average values", rtx_options.h rtx.displacement.mode).
pbr::Image nextMip(const pbr::Image& src, bool maxFilter) {
  pbr::Image dst;
  dst.width = std::max(1u, src.width / 2);
  dst.height = std::max(1u, src.height / 2);
  dst.rgba.resize(size_t(dst.width) * dst.height * 4);
  for (uint32_t y = 0; y < dst.height; ++y) {
    for (uint32_t x = 0; x < dst.width; ++x) {
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0, peak = 0;
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t sx = std::min(src.width - 1, x * 2 + (k & 1)), sy = std::min(src.height - 1, y * 2 + (k >> 1));
          const uint8_t v = src.rgba[(size_t(sy) * src.width + sx) * 4 + c];
          sum += v;
          peak = std::max<uint32_t>(peak, v);
        }
        dst.rgba[(size_t(y) * dst.width + x) * 4 + c] = static_cast<uint8_t>(maxFilter ? peak : (sum + 2) / 4);
      }
    }
  }
  return dst;
}

uint64_t uploadPbrImage(const pbr::Image& image, PbrMap kind) {
  if (image.empty()) {
    return 0;
  }
  std::vector<uint8_t> data;
  uint32_t levels = 0;
  pbr::Image level = image;
  for (;;) {
    ++levels;
    std::vector<uint8_t> bgra(level.rgba.size());
    for (size_t i = 0; i < size_t(level.width) * level.height; ++i) {
      const uint8_t* s = &level.rgba[i * 4];
      uint8_t* d = &bgra[i * 4];
      if (kind == PbrMap::kGray || kind == PbrMap::kHeight) {
        d[0] = d[1] = d[2] = s[0];  // grey maps are read from .r
        d[3] = 255;
      } else {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
    if (kind == PbrMap::kNormal) {
      // Workflow normals are OpenGL (green = up = towards -v), the engine's convention (B opposite to dP/dv), so the
      // engine normal-map conversion applies unchanged.
      convertNormalLevel(bgra, level.width, level.height, D3DFMT_A8R8G8B8, data);
    } else {
      data.insert(data.end(), bgra.begin(), bgra.end());
    }
    if (level.width == 1 && level.height == 1) {
      break;
    }
    level = nextMip(level, kind == PbrMap::kHeight);
  }
  const uint64_t hash = 0xAC1B000000000000ull | (uint64_t(++s_textureSerial) << 16);
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = hash;
  info.width = image.width;
  info.height = image.height;
  info.depth = 1;
  info.mipLevels = levels;
  info.format = kind == PbrMap::kNormal ? REMIXAPI_FORMAT_BC5_UNORM : REMIXAPI_FORMAT_B8G8R8A8_UNORM;
  info.data = data.data();
  info.dataSize = data.size();
  remixapi_TextureHandle handle = nullptr;
  const remixapi_ErrorCode err = s_api.CreateTexture(&info, &handle);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
    log::line("pbr: CreateTexture %ux%u kind %d -> %d", image.width, image.height, static_cast<int>(kind), err);
    return 0;
  }
  return hash;
}

// Block-compressed maps from tools/pbr_pack.py: the mip chain goes to Remix unchanged.
uint64_t uploadPackedPbr(const pbr::Packed& p) {
  if (p.empty()) {
    return 0;
  }
  const uint64_t hash = 0xAC1B000000000000ull | (uint64_t(++s_textureSerial) << 16);
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = hash;
  info.width = p.width;
  info.height = p.height;
  info.depth = 1;
  info.mipLevels = p.levels;
  info.format = static_cast<remixapi_Format>(p.format);
  info.data = p.data.data();
  info.dataSize = p.data.size();
  remixapi_TextureHandle handle = nullptr;
  const remixapi_ErrorCode err = s_api.CreateTexture(&info, &handle);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
    log::line("pbr: CreateTexture packed %ux%u fmt %u mips %u -> %d", p.width, p.height, p.format, p.levels, err);
    return 0;
  }
  s_pbrHandles[hash] = handle;
  return hash;
}

void destroyPbrTexture(uint64_t hash) {
  const auto it = s_pbrHandles.find(hash);
  if (it != s_pbrHandles.end()) {
    s_api.DestroyTexture(it->second);
    s_pbrHandles.erase(it);
  }
}

void uploadPackedSet(pbr::Override& maps, PbrTextures& t) {
  t.albedo = uploadPackedPbr(maps.packed[pbr::kAlbedo]);
  t.normal = uploadPackedPbr(maps.packed[pbr::kNormal]);
  t.roughness = uploadPackedPbr(maps.packed[pbr::kRoughness]);
  t.metallic = uploadPackedPbr(maps.packed[pbr::kMetallic]);
  t.height = uploadPackedPbr(maps.packed[pbr::kHeight]);
  t.heightMetres = maps.heightMetres;
  t.reliefUv = maps.reliefUv;
  t.pomDefault = maps.pomDefault;
  t.metallicConstant = maps.metallicConstant;
}

// <pbr>\live_textures.txt for the runtime's material editor: "<Remix texture hash> <content hash> <P|E|U>" (P = PBR
// albedo, E = engine albedo of a texture that has PBR maps, U = engine albedo of a surface that takes PBR maps but has
// none yet), so a picked surface resolves to its maps folder and the editor can show the unconverted ones.
std::unordered_map<uint64_t, float> s_autoDepthCm;  // content hash -> automatic POM depth (cm) of its last material

void writeLiveTextures() {
  if (!pbr::enabled()) {
    return;
  }
  FILE* f = _wfopen((pbr::folder() + L"\\live_textures.txt").c_str(), L"w");
  if (!f) {
    return;
  }
  for (const auto& [content, t] : s_pbrTextures) {
    if (t.albedo) {
      const auto depth = s_autoDepthCm.find(content);
      fprintf(f, "%016llX %016llX P %.2f\n", static_cast<unsigned long long>(t.albedo),
              static_cast<unsigned long long>(content), depth != s_autoDepthCm.end() ? depth->second : 0.0f);
    }
  }
  for (const auto& [tex, entry] : s_textures) {
    const auto pbrEntry = s_pbrTextures.find(entry.contentHash);
    // pbrFor adds an entry for every albedo of a surface eligible for PBR, with or without maps
    if (entry.valid && pbrEntry != s_pbrTextures.end()) {
      fprintf(f, "%016llX %016llX %c\n", static_cast<unsigned long long>(entry.hash),
              static_cast<unsigned long long>(entry.contentHash), pbrEntry->second.albedo ? 'E' : 'U');
    }
  }
  fclose(f);
}

const PbrTextures* pbrFor(IDirect3DTexture9* albedo) {
  if (!pbr::enabled() || !ensureTexture(albedo)) {
    return nullptr;
  }
  const uint64_t content = s_textures[albedo].contentHash;
  auto it = s_pbrTextures.find(content);
  if (it == s_pbrTextures.end()) {
    PbrTextures t;
    if (auto maps = pbr::load(content); maps && maps->isPacked) {
      t.editStamp = pbr::editStamp(content);
      t.edit = pbr::readEdit(content);
      pbr::applyEdit(*maps, t.edit);
      uploadPackedSet(*maps, t);
      t.valid = t.albedo != 0 && t.edit.enabled;
    } else if (maps) {
      t.albedo = uploadPbrImage(maps->albedo, PbrMap::kColor);
      t.normal = uploadPbrImage(maps->normal, PbrMap::kNormal);
      t.roughness = uploadPbrImage(maps->roughness, PbrMap::kGray);
      t.metallic = uploadPbrImage(maps->metallic, PbrMap::kGray);
      t.height = uploadPbrImage(maps->height, PbrMap::kHeight);
      t.heightMetres = maps->heightMetres;
      t.reliefUv = maps->reliefUv;
      t.pomDefault = maps->pomDefault;
      t.metallicConstant = maps->metallicConstant;
      t.valid = t.albedo != 0;
    }
    it = s_pbrTextures.emplace(content, t).first;
    writeLiveTextures();
  }
  return it->second.valid ? &it->second : nullptr;
}

// PBR-eligible albedo (createSurfaceMaterial) -> pbr dump\<content hash>.png and its engine normal map -> <content hash>_normal.png
// (level 0), inputs of the offline workflow.
std::set<uint64_t> s_pbrDumpedNormal;

void dumpPbrSource(IDirect3DTexture9* albedo, IDirect3DTexture9* normal) {
  if (!pbr::dumpEnabled() || !ensureTexture(albedo)) {
    return;
  }
  const uint64_t content = s_textures[albedo].contentHash;
  std::vector<uint8_t> rgb, alpha;
  uint32_t w = 0, h = 0;
  if (s_pbrDumped.insert(content).second && readLevelRgb(albedo, 0, rgb, w, h, &alpha)) {
    pbr::dump(content, L"", w, h, rgb, &alpha);  // alpha kept for alpha-tested / blended surfaces
  }
  if (normal && s_pbrDumpedNormal.insert(content).second && readLevelRgb(normal, 0, rgb, w, h)) {
    pbr::dump(content, L"_normal", w, h, rgb);
  }
}

// Metres of surface per texture repeat, per engine (mesh, range): median over triangle edges of |dP| / |dUV|.
// Remix's POM depth (displaceIn) is in texture-coordinate units (pomGetStep: step = dir.xy / dir.z * totalHeight in
// texcoords), so a depth in metres is divided by this.
std::map<std::pair<const void*, uint32_t>, float> s_metresPerUv;
std::unordered_map<uint64_t, float> s_surfaceMetresPerUv;  // material hash -> metres per UV
// Whether the texture repeats over the surface (uv span > 1): atlas-mapped props and characters do not, and POM there
// slides the texture across island borders.
std::map<std::pair<const void*, uint32_t>, bool> s_uvTiles;
std::unordered_map<uint64_t, bool> s_surfaceUvTiles;       // material hash -> uv tiles
std::unordered_set<uint64_t> s_skinnedSurfaces;             // material hashes of skinned and dynamic meshes (no PBR)

// Any vertex with position[3] and texcoord[2] (static, skinned and dynamic meshes share the layout).
template <typename Vertex, typename Index>
float measureMetresPerUv(const std::vector<Vertex>& vertices, const std::vector<Index>& indices) {
  std::vector<float> ratios;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    for (int e = 0; e < 3; ++e) {
      const auto& a = vertices[indices[i + e]];
      const auto& b = vertices[indices[i + (e + 1) % 3]];
      const float dp = std::sqrt((a.position[0] - b.position[0]) * (a.position[0] - b.position[0]) +
                                 (a.position[1] - b.position[1]) * (a.position[1] - b.position[1]) +
                                 (a.position[2] - b.position[2]) * (a.position[2] - b.position[2]));
      const float du = std::hypot(a.texcoord[0] - b.texcoord[0], a.texcoord[1] - b.texcoord[1]);
      if (du > 1e-5f && dp > 1e-5f) {
        ratios.push_back(dp / du);
      }
    }
  }
  if (ratios.empty()) {
    return 0.0f;
  }
  std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
  return ratios[ratios.size() / 2];
}

float measureMetresPerUv(const static_geometry::Surface& surface) {
  return measureMetresPerUv(surface.vertices, surface.indices);
}

// Measured once per (engine mesh, range): every surface that can take PBR maps needs it for POM (createSurfaceMaterial).
template <typename Vertex, typename Index>
void ensureMetresPerUv(const void* mesh, uint32_t range, const std::vector<Vertex>& vertices,
                       const std::vector<Index>& indices) {
  if (!s_metresPerUv.count({ mesh, range })) {
    s_metresPerUv[{ mesh, range }] = measureMetresPerUv(vertices, indices);
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (Index i : indices) {
      for (int c = 0; c < 2; ++c) {
        lo[c] = std::min(lo[c], vertices[i].texcoord[c]);
        hi[c] = std::max(hi[c], vertices[i].texcoord[c]);
      }
    }
    s_uvTiles[{ mesh, range }] = hi[0] - lo[0] > 1.02f || hi[1] - lo[1] > 1.02f;
  }
}

// ---- Specular -> roughness. Every engine lobe is Phong pow(sat(dot(reflect(-L, N), V)), n) (docs/notes/specular.md).
// Remix opaque materials only take a perceptual roughness (squared into the GGX alpha, brdf.slangh:65-67) with a fixed
// dielectric F0 of 0.04 (brdf.slangh:36), so only the exponent is translated: Phong n -> Blinn-Phong 4n -> Beckmann
// alpha = sqrt(2 / (4n + 2)) (Walter et al. 2007, sec. 5.2) -> r = sqrt(alpha). Shaders without a Phong term get
// r = 1 (the closest Remix has to "no specular lobe"). Intensity, colour and mask of the engine term have no input.
float roughnessFromPhong(float n) {
  if (!(n > 0.0f)) {
    return 1.0f;
  }
  return std::clamp(std::pow(2.0f / (4.0f * n + 2.0f), 0.25f), 0.0f, 1.0f);
}

using RoughnessKey = std::tuple<IDirect3DTexture9*, uint8_t, uint8_t, float>;  // (spec map, mode, channel, param)
std::map<RoughnessKey, RemixTexture> s_roughnessTextures;
std::unordered_map<IDirect3DTexture9*, uint32_t> s_roughnessPins;  // spec map -> our AddRef (one per map)

// Per-texel roughness from the spec map channel the PS feeds into the exponent: n = T + param (kTexturePlusConstant)
// or n = T * param (kTextureTimesK), T = channel / 255 (UNORM sampling).
uint64_t ensureRoughnessTexture(IDirect3DTexture9* tex, uint8_t mode, uint8_t channel, float param) {
  const RoughnessKey key { tex, mode, channel, param };
  auto it = s_roughnessTextures.find(key);
  if (it != s_roughnessTextures.end()) {
    return it->second.valid ? it->second.hash : 0;
  }
  RemixTexture entry;
  D3DSURFACE_DESC desc = {};
  remixapi_Format ignored = {};
  if (channel <= 2 && SUCCEEDED(tex->GetLevelDesc(0, &desc)) && remixFormatFor(desc.Format, ignored)) {
    const bool plus = mode == 1 + static_cast<uint8_t>(specular_table::Exponent::kTexturePlusConstant);
    const UINT levels = tex->GetLevelCount();
    std::vector<uint8_t> data;
    bool ok = true;
    for (UINT l = 0; l < levels && ok; ++l) {
      D3DSURFACE_DESC level = {};
      std::vector<uint8_t> src;
      ok = SUCCEEDED(tex->GetLevelDesc(l, &level)) && appendLevel(tex, l, desc.Format, src);
      if (!ok) {
        break;
      }
      const uint32_t w = level.Width, h = level.Height;
      const size_t base = data.size();
      data.resize(base + size_t(w) * h * 4);
      auto store = [&](uint32_t x, uint32_t y, uint8_t value) {
        const float t = value / 255.0f;
        const float r = roughnessFromPhong(plus ? t + param : t * param);
        const uint8_t out = static_cast<uint8_t>(std::lround(r * 255.0f));
        uint8_t* p = data.data() + base + (size_t(y) * w + x) * 4;
        p[0] = p[1] = p[2] = out;
        p[3] = 255;
      };
      if (isBlockCompressed(desc.Format)) {
        const uint32_t blockBytes = desc.Format == D3DFMT_DXT1 ? 8 : 16;
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (uint32_t by = 0; by < bh; ++by) {
          for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t rgb[16][3];
            const uint8_t* block = src.data() + (size_t(by) * bw + bx) * blockBytes + (blockBytes == 16 ? 8 : 0);
            decodeColorBlock(block, desc.Format == D3DFMT_DXT1, rgb);
            for (int t = 0; t < 16; ++t) {
              const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
              if (x < w && y < h) {
                store(x, y, rgb[t][channel]);
              }
            }
          }
        }
      } else {  // BGRA8 from appendLevel
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            store(x, y, src[(size_t(y) * w + x) * 4 + (2 - channel)]);
          }
        }
      }
    }
    if (ok) {
      entry.hash = 0xAC1B000000000000ull | (uint64_t(++s_textureSerial) << 32) | reinterpret_cast<uintptr_t>(tex);
      remixapi_TextureInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      info.hash = entry.hash;
      info.width = desc.Width;
      info.height = desc.Height;
      info.depth = 1;
      info.mipLevels = levels;
      info.format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
      info.data = data.data();
      info.dataSize = data.size();
      const remixapi_ErrorCode err = s_api.CreateTexture(&info, &entry.handle);
      entry.valid = err == REMIXAPI_ERROR_CODE_SUCCESS;
      if (!entry.valid) {
        log::line("roughness texture from %p fmt=%u %ux%u channel %u %s %.3f -> %d", tex, desc.Format, desc.Width,
                  desc.Height, channel, plus ? "+" : "*", param, err);
      }
    }
  } else {
    log::line("spec map %p format %u channel %u not converted", tex, desc.Format, channel);
  }
  if (s_roughnessPins[tex]++ == 0) {
    tex->AddRef();
  }
  s_roughnessTextures.emplace(key, entry);
  return entry.valid ? entry.hash : 0;
}
// ---- Texture release. The mod pins every uploaded texture with AddRef; once every remaining reference is the
// mod's own (engine unloaded it: the bridge client's Release returns the interface refcount, base.h:425-445), the
// Remix texture is destroyed and the pin dropped. Materials that already captured it keep their image alive.
void collectUnusedTextures() {
  static uint32_t s_counter = 0;
  if (++s_counter % 120 != 0) {
    return;
  }
  uint32_t released = 0;
  for (auto* map : { &s_textures, &s_normalTextures }) {
    for (auto it = map->begin(); it != map->end();) {
      IDirect3DTexture9* tex = it->first;
      const ULONG total = tex->AddRef() - 1;
      tex->Release();
      const uint32_t ours = (s_textures.count(tex) ? 1u : 0u) + (s_normalTextures.count(tex) ? 1u : 0u) +
                            (s_roughnessPins.count(tex) ? 1u : 0u) + skinPins(tex) + materials::heldReferences(tex);
      if (total > ours) {
        ++it;
        continue;
      }
      if (it->second.valid && it->second.handle) {
        s_api.DestroyTexture(it->second.handle);
      }
      it = map->erase(it);
      tex->Release();
      ++released;
    }
  }
  for (auto pin = s_roughnessPins.begin(); pin != s_roughnessPins.end();) {
    IDirect3DTexture9* tex = pin->first;
    const ULONG total = tex->AddRef() - 1;
    tex->Release();
    const uint32_t ours = (s_textures.count(tex) ? 1u : 0u) + (s_normalTextures.count(tex) ? 1u : 0u) + 1u +
                          skinPins(tex) + materials::heldReferences(tex);
    if (total > ours) {
      ++pin;
      continue;
    }
    for (auto r = s_roughnessTextures.lower_bound({ tex, uint8_t(0), uint8_t(0), -1e30f });
         r != s_roughnessTextures.end() && std::get<0>(r->first) == tex;) {
      if (r->second.valid && r->second.handle) {
        s_api.DestroyTexture(r->second.handle);
      }
      r = s_roughnessTextures.erase(r);
    }
    pin = s_roughnessPins.erase(pin);
    tex->Release();
    ++released;
  }
  // Skin composites go when the engine dropped any of their sources.
  for (auto skin = s_skinTextures.begin(); skin != s_skinTextures.end();) {
    bool unused = false;
    for (IDirect3DTexture9* tex : { std::get<0>(skin->first), std::get<1>(skin->first), std::get<2>(skin->first) }) {
      if (tex) {
        const ULONG total = tex->AddRef() - 1;
        tex->Release();
        const uint32_t ours = (s_textures.count(tex) ? 1u : 0u) + (s_normalTextures.count(tex) ? 1u : 0u) +
                              (s_roughnessPins.count(tex) ? 1u : 0u) + skinPins(tex) +
                              materials::heldReferences(tex);
        unused = unused || total <= ours;
      }
    }
    if (!unused) {
      ++skin;
      continue;
    }
    if (skin->second.valid && skin->second.handle) {
      s_api.DestroyTexture(skin->second.handle);
    }
    for (IDirect3DTexture9* tex : { std::get<0>(skin->first), std::get<1>(skin->first), std::get<2>(skin->first) }) {
      if (tex) {
        if (--s_skinPins[tex] == 0) {
          s_skinPins.erase(tex);
        }
        tex->Release();
      }
    }
    skin = s_skinTextures.erase(skin);
    ++released;
  }
  if (released) {
    log::line("released %u textures no longer referenced by the engine (live %zu albedo, %zu normal)", released,
              s_textures.size(), s_normalTextures.size());
  }
}

// ---- Per-surface materials. Each primitive range of an engine mesh gets its own Remix material hash; the
// material is re-created with the same hash when the colour pass reveals (or changes) its textures, so meshes
// never need to be rebuilt (the runtime resolves a surface's material by hash at every draw).

// Skinned surfaces are keyed by MaterialInstance too (materials.h Update::matInst); static ones use nullptr.
using SurfaceKey = std::tuple<const void*, uint32_t, const void*>;  // (DX9StaticMesh*, range, skinned matInst)
std::map<SurfaceKey, materials::SurfaceMaterial> s_surfaceState;
std::unordered_map<uint64_t, remixapi_MaterialHandle> s_surfaceMaterials;  // material hash -> handle
std::unordered_map<uint64_t, uint8_t> s_surfaceBlend;                      // material hash -> engine blend mode
std::unordered_map<uint64_t, uint8_t> s_surfaceLayer;                      // material hash -> terrain layer (1/2)
std::unordered_map<const void*, uint64_t> s_meshKeyOf;                    // DX9StaticMesh* -> live mesh key
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, const void*>>> s_meshRanges;  // mesh key -> (range, matInst)

uint64_t surfaceMaterialHash(uint64_t meshKey, uint32_t range, const void* matInst, uint8_t layer = 0) {
  uint64_t h = meshKey * 0x9E3779B97F4A7C15ull ^ (uint64_t(range) + 1) * 0xC2B2AE3D27D4EB4Full ^
               (uint64_t(reinterpret_cast<uintptr_t>(matInst)) + 1) * 0x165667B19E3779F9ull ^
               uint64_t(layer) * 0xD6E8FEB86659FD93ull;
  h ^= h >> 29;
  return (h & 0x0000FFFFFFFFFFFFull) | 0xAC1D000000000000ull;
}

// ---- Vegetation export ([PBR] ExportVegetation=1, user 2026-10-03: "save the vegetation assets, we will rebuild
// them"). Into <pbr>\vegetation: <id>.obj (local space, uv with v flipped, the mod's Remix winding),
// textures\<content>.png / <content>_normal.png (level 0, albedo with alpha), index.txt (one line per mesh: id, kind,
// albedo and normal content hashes, alpha test) and placements.txt (id + 3x4 world matrix, one line per distinct
// position). Exported: the instanced clutter (grass) and every alpha-tested static surface (foliage, palms, ivy;
// fences and lattices too, to be sorted when rebuilding).
std::set<uint64_t> s_vegExported;
std::set<std::tuple<uint64_t, int, int, int>> s_vegPlaced;
std::unordered_map<uint64_t, std::vector<uint64_t>> s_vegOfMesh;  // static mesh key -> exported geometry ids
struct VegPending {
  uint64_t meshKey = 0;
  std::vector<static_geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
};
std::map<std::pair<const void*, uint32_t>, VegPending> s_vegPending;  // static ranges whose material is not known yet

std::wstring vegFolder() {
  return pbr::folder() + L"\\vegetation";
}

uint64_t vegTexture(IDirect3DTexture9* tex, const wchar_t* suffix) {
  if (!tex || !ensureTexture(tex)) {
    return 0;
  }
  const uint64_t content = s_textures[tex].contentHash;
  wchar_t name[64];
  swprintf_s(name, L"\\textures\\%016llX%ls.png", static_cast<unsigned long long>(content), suffix);
  const std::wstring path = vegFolder() + name;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::vector<uint8_t> rgb, alpha;
    uint32_t w = 0, h = 0;
    if (readLevelRgb(tex, 0, rgb, w, h, &alpha)) {
      pbr::writePng(path, w, h, rgb, &alpha);
    }
  }
  return content;
}

template <typename Vertex, typename Index>
uint64_t vegGeometryId(const std::vector<Vertex>& vertices, const std::vector<Index>& indices) {
  uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001B3ull;
  };
  for (const auto& v : vertices) {
    mix(v.position, sizeof(v.position));
    mix(v.texcoord, sizeof(v.texcoord));
  }
  mix(indices.data(), indices.size() * sizeof(Index));
  return h;
}

template <typename Vertex, typename Index>
void vegExport(uint64_t id, const char* kind, const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
               const materials::SurfaceMaterial* m) {
  if (!pbr::exportVegetation() || !s_vegExported.insert(id).second) {
    return;
  }
  CreateDirectoryW(vegFolder().c_str(), nullptr);
  CreateDirectoryW((vegFolder() + L"\\textures").c_str(), nullptr);
  const uint64_t albedo = m ? vegTexture(m->albedo, L"") : 0;
  const uint64_t normal = m ? vegTexture(m->normal, L"_normal") : 0;
  wchar_t name[64];
  swprintf_s(name, L"\\%016llX.obj", static_cast<unsigned long long>(id));
  if (FILE* f = _wfopen((vegFolder() + name).c_str(), L"w")) {
    fprintf(f, "# AC1 RTX vegetation export: %s, albedo textures/%016llX.png, normal textures/%016llX_normal.png\n",
            kind, static_cast<unsigned long long>(albedo), static_cast<unsigned long long>(normal));
    for (const auto& v : vertices) fprintf(f, "v %.6f %.6f %.6f\n", v.position[0], v.position[1], v.position[2]);
    for (const auto& v : vertices) fprintf(f, "vt %.6f %.6f\n", v.texcoord[0], 1.0f - v.texcoord[1]);
    for (const auto& v : vertices) fprintf(f, "vn %.6f %.6f %.6f\n", v.normal[0], v.normal[1], v.normal[2]);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      const unsigned a = unsigned(indices[i]) + 1, b = unsigned(indices[i + 1]) + 1, c = unsigned(indices[i + 2]) + 1;
      fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
    }
    fclose(f);
  }
  if (FILE* f = _wfopen((vegFolder() + L"\\index.txt").c_str(), L"a")) {
    fprintf(f, "%016llX %s vertices %zu triangles %zu albedo %016llX normal %016llX alphatest %d alpharef %u\n",
            static_cast<unsigned long long>(id), kind, vertices.size(), indices.size() / 3,
            static_cast<unsigned long long>(albedo), static_cast<unsigned long long>(normal),
            m && m->alphaTest ? 1 : 0, m ? unsigned(m->alphaRef) : 0u);
    fclose(f);
  }
  log::line("vegetation: exported %016llX (%s, %zu triangles)", static_cast<unsigned long long>(id), kind,
            indices.size() / 3);
}

void vegPlace(uint64_t id, const remixapi_Transform& t) {
  if (!pbr::exportVegetation()) {
    return;
  }
  const auto key = std::make_tuple(id, int(std::lround(t.matrix[0][3] * 100.0f)), int(std::lround(t.matrix[1][3] * 100.0f)),
                                   int(std::lround(t.matrix[2][3] * 100.0f)));
  if (!s_vegPlaced.insert(key).second) {
    return;
  }
  if (FILE* f = _wfopen((vegFolder() + L"\\placements.txt").c_str(), L"a")) {
    fprintf(f, "%016llX", static_cast<unsigned long long>(id));
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) fprintf(f, " %.5f", t.matrix[r][c]);
    }
    fprintf(f, "\n");
    fclose(f);
  }
}

// ---- Bushes replaced by the vegetation scatter (user 2026-10-03: "there are various bushes in the game, remove them
// and put our plants there"): <pbr>\vegetation\scatter\removed.txt lists exported static geometry ids
// (tools/veg_scatter_plan.py: the leafy shrub cards). A static mesh made only of such ranges is not drawn while a
// scatter plan is active (its level, F7 not pressed); the plan has plants where they stood.
std::unordered_map<uint64_t, std::map<uint32_t, uint64_t>> s_vegRangeIds;  // mesh key -> alpha-tested range -> id
std::unordered_map<uint64_t, std::set<uint32_t>> s_meshAllRanges;         // mesh key -> every (non-layer) range

const std::set<uint64_t>& vegRemoved() {
  static std::set<uint64_t> s_removed;
  static bool s_loaded = false;
  if (!s_loaded && !pbr::folder().empty()) {
    s_loaded = true;
    if (FILE* f = _wfopen((pbr::folder() + L"\\vegetation\\scatter\\removed.txt").c_str(), L"r")) {
      unsigned long long id = 0;
      while (fscanf_s(f, "%llx", &id) == 1) s_removed.insert(id);
      fclose(f);
    }
    log::line("vegetation: %zu static geometries replaced by the scatter", s_removed.size());
  }
  return s_removed;
}

void vegRecordRange(uint64_t meshKey, uint32_t range, uint64_t id) {
  auto& ids = s_vegOfMesh[meshKey];
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  s_vegRangeIds[meshKey][range] = id;
}

bool scatterLevelActive();

bool vegMeshRemoved(uint64_t meshKey) {
  if (vegRemoved().empty() || !scatterLevelActive()) {
    return false;
  }
  const auto all = s_meshAllRanges.find(meshKey);
  const auto veg = s_vegRangeIds.find(meshKey);
  if (all == s_meshAllRanges.end() || veg == s_vegRangeIds.end() || all->second.empty()) {
    return false;
  }
  for (const uint32_t range : all->second) {
    const auto id = veg->second.find(range);
    if (id == veg->second.end() || !vegRemoved().count(id->second)) {
      return false;
    }
  }
  return true;
}

// Static range: exported when its material is alpha-tested (now, or when the material arrives).
void vegStaticRange(uint64_t meshKey, const static_geometry::Surface& src) {
  s_meshAllRanges[meshKey].insert(src.range);
  if (!pbr::exportVegetation() && vegRemoved().empty()) {
    return;
  }
  const void* staticMesh = staticMeshOf(meshKey);
  const auto state = s_surfaceState.find({ staticMesh, src.range, nullptr });
  if (state == s_surfaceState.end()) {
    s_vegPending[{ staticMesh, src.range }] = { meshKey, src.vertices, src.indices };
    return;
  }
  if (state->second.alphaTest) {
    const uint64_t id = vegGeometryId(src.vertices, src.indices);
    vegExport(id, "static", src.vertices, src.indices, &state->second);
    vegRecordRange(meshKey, src.range, id);
  }
}

// Baked shadow decals: alpha-blended albedos that are black wherever they are visible (AC1 stamps pre-rendered
// silhouettes of furniture, columns and window frames on the floors, e.g. E72B29CC, 686C78C3, B334DB09). Path tracing
// already casts those shadows, so drawn they double-darken with edges that do not match ("flakes" in the column's
// shadow, 2026-10-03). Read from the texture (a small mip level), cached per texture.
std::unordered_map<IDirect3DTexture9*, bool> s_shadowDecal;

bool isShadowDecal(IDirect3DTexture9* tex) {
  const auto it = s_shadowDecal.find(tex);
  if (it != s_shadowDecal.end()) {
    return it->second;
  }
  std::vector<uint8_t> rgb, alpha;
  uint32_t w = 0, h = 0;
  bool shadow = false;
  if (readLevelRgb(tex, matchingLevel(tex, 64), rgb, w, h, &alpha) && !alpha.empty()) {
    size_t visible = 0, dark = 0;
    uint8_t maxAlpha = 0;
    double sum[3] = {}, weight = 0.0;
    for (size_t i = 0; i < alpha.size(); ++i) {
      maxAlpha = std::max(maxAlpha, alpha[i]);
      for (int c = 0; c < 3; ++c) sum[c] += double(rgb[i * 3 + c]) * alpha[i];
      weight += alpha[i];
      if (alpha[i] > 8) {
        ++visible;
        dark += std::max({ rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2] }) <= 10 ? 1 : 0;
      }
    }
    shadow = visible * 100 >= alpha.size() && dark * 100 >= visible * 99;
    // Faint grime / occlusion overlays (9BB978AF: colour ~30/255, alpha <= 32 %, drawn by the engine only from afar):
    // dark, grey-brown and never more than ~35 % opaque. Blood and coloured stains (saturated) and real decals
    // (opaque somewhere) are kept.
    if (!shadow && weight > 0.0 && maxAlpha <= 90) {
      const double r = sum[0] / weight / 255.0, g = sum[1] / weight / 255.0, b = sum[2] / weight / 255.0;
      const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      shadow = luma <= 0.15 && std::max({ r, g, b }) - std::min({ r, g, b }) <= 0.06;
    }
  }
  s_shadowDecal[tex] = shadow;
  if (shadow) {
    log::line("texture %p: baked shadow / grime decal, hidden", tex);
  }
  return shadow;
}

remixapi_MaterialHandle createSurfaceMaterial(uint64_t hash, const materials::SurfaceMaterial* m) {
  const auto blendIt = s_surfaceBlend.find(hash);
  uint8_t blend = blendIt != s_surfaceBlend.end() ? blendIt->second : 0;
  const auto layerIt = s_surfaceLayer.find(hash);
  const uint8_t layer = layerIt != s_surfaceLayer.end() ? layerIt->second : 0;
  // Terrain layer overlays: the layer's albedo and normal map, alpha-blended by the vertex weight (drawStatic).
  IDirect3DTexture9* albedoTex = m ? m->albedo : nullptr;
  IDirect3DTexture9* normalTex = m ? m->normal : nullptr;
  if (layer) {
    albedoTex = (m && m->layered) ? m->layerAlbedo[layer - 1] : nullptr;
    normalTex = (m && m->layered) ? m->layerNormal[layer - 1] : nullptr;
    blend = game::kBlendAlpha;
  }
  // PBR override for surfaces with a plain engine albedo (not skin, water or glass), foliage and alpha-blended ones
  // included (user 2026-10-03: maybe redone later): the dump keeps the albedo's alpha, the workflow carries it to the
  // PBR albedo (BC3). Additive / screen / multiply effects keep the engine texture. Every eligible albedo is dumped
  // for the workflow (since 2026-10-03, before only the layered terrain).
  // Skinned and dynamic meshes (characters, cloth, clutter) keep the engine materials (user 2026-10-03: "the
  // characters are wrong", "Altair's skirt is grey"): atlases and PS-tinted textures, not the tiling surfaces the
  // workflow is made for.
  const PbrTextures* pbrMaps = nullptr;
  if (albedoTex && m && !m->skin && !m->water && !m->glass && blend != game::kBlendGlass &&
      !s_skinnedSurfaces.count(hash) && (layer || !blend || blend == game::kBlendAlpha)) {
    dumpPbrSource(albedoTex, normalTex);
    pbrMaps = pbrFor(albedoTex);
  }
  std::wstring albedoPath;
  if (pbrMaps && pbrMaps->edit.map[pbr::kAlbedo].enabled) {
    albedoPath = texturePath(pbrMaps->albedo);
  } else if (albedoTex) {  // no maps, or the editor hid the PBR albedo: the engine's
    const uint64_t texHash = (!layer && m->skin) ? ensureSkinTexture(*m) : ensureTexture(albedoTex);
    if (texHash) {
      albedoPath = texturePath(texHash);
    }
  }
  std::wstring normalPath;
  if (pbrMaps && pbrMaps->normal) {
    normalPath = texturePath(pbrMaps->normal);
  } else if (normalTex) {
    const uint64_t texHash = ensureNormalTexture(normalTex);
    if (texHash) {
      normalPath = texturePath(texHash);
    }
  }
  if (!layer && m && (m->water || (blend == game::kBlendGlass && m->glass))) {
    // Engine mode 5 = DESTCOLOR/ONE: the background passes unattenuated (factor 1 + src >= 1) and the surface only
    // adds its lit/reflected term, with no ray bending -> thin-walled translucent, transmittance 1. The PS has no
    // physical reflectance: Fresnel_17 (c133, measured 5/15/20) scales the specular at normal incidence
    // (lerp(c133, 1, (1 - N.V)^5)), so the IOR is Remix's own translucent default (rtx_material_data.h:110, 1.3).
    remixapi_MaterialInfoTranslucentEXT translucent = {};
    translucent.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    translucent.refractiveIndex = 1.3f;
    translucent.transmittanceColor = { 1.0f, 1.0f, 1.0f };
    if (m->water) {
      // Water (game.h kWaterPs): the raster draws a tinted environment reflection over the background; as a Remix
      // translucent surface the reflection comes from the path tracer (Fresnel, sharp, rippled by the normal map)
      // and the background is seen through it, tinted by Color_9 (the game's water tint).
      translucent.transmittanceColor = { std::clamp(m->waterColor[0], 0.0f, 1.0f),
                                         std::clamp(m->waterColor[1], 0.0f, 1.0f),
                                         std::clamp(m->waterColor[2], 0.0f, 1.0f) };
    }
    translucent.transmittanceMeasurementDistance = 1.0f;
    translucent.thinWallThickness_hasvalue = 1;
    translucent.thinWallThickness_value = 0.001f;  // no effect with transmittance 1 (no absorption)
    translucent.useDiffuseLayer = 0;
    remixapi_MaterialInfo material = {};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.pNext = &translucent;
    material.hash = hash;
    material.normalTexture = normalPath.empty() ? nullptr : normalPath.c_str();
    material.spriteSheetRow = 1;
    material.spriteSheetCol = 1;
    material.filterMode = 1;
    material.wrapModeU = 1;
    material.wrapModeV = 1;
    remixapi_MaterialHandle handle = nullptr;
    ++s_materialsCreated;
    if (s_api.CreateMaterial(&material, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
      return nullptr;
    }
    s_surfaceMaterials[hash] = handle;
    return handle;
  }
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = { 0.5f, 0.5f, 0.5f };
  opaque.opacityConstant = 1.0f;
  // Engine specular -> roughness (see roughnessFromPhong). PS not in the census: previous default 0.7 (not data).
  std::wstring roughnessPath;
  opaque.roughnessConstant = 0.7f;
  if (m && m->specMode == 1 + static_cast<uint8_t>(specular_table::Exponent::kNone)) {
    opaque.roughnessConstant = 1.0f;
  } else if (m && m->specMode == 1 + static_cast<uint8_t>(specular_table::Exponent::kConstant)) {
    opaque.roughnessConstant = roughnessFromPhong(m->specParam);
  } else if (m && m->specTexture && m->specMode > 2) {
    const uint64_t texHash = ensureRoughnessTexture(m->specTexture, m->specMode, m->specChannel, m->specParam);
    if (texHash) {
      roughnessPath = texturePath(texHash);
      opaque.roughnessTexture = roughnessPath.c_str();
    }
  }
  std::wstring metallicPath, heightPath;
  if (pbrMaps) {
    if (pbrMaps->roughness) {
      roughnessPath = texturePath(pbrMaps->roughness);
      opaque.roughnessTexture = roughnessPath.c_str();
    }
    if (pbrMaps->metallic) {
      metallicPath = texturePath(pbrMaps->metallic);
      opaque.metallicTexture = metallicPath.c_str();
    } else if (pbrMaps->metallicConstant > 0.0f) {
      opaque.metallicConstant = pbrMaps->metallicConstant;
    }
    // POM on opaque surfaces; terrain layer overlays (coplanar alpha-blended decals, still Remix opaque-type
    // materials, geometry_resolver.slangh tests only materialType == opaque) only with [PBR] LayerPom (test).
    // Not on alpha-tested surfaces (POM slides the texture, not the cut-out) nor where the texture does not repeat
    // (atlas islands). Remix's displacement is in texture units, so the automatic depth is the material class's
    // relief per texture repeat (tools/pbr_pack.py), capped at 4 % of a repeat; the editor's depth is in cm.
    const auto mpu = s_surfaceMetresPerUv.find(hash);
    const auto tiles = s_surfaceUvTiles.find(hash);
    const bool pomAllowed = (layer ? pbr::layerPom() : !blend) && !(m->alphaTest && !layer) &&
                            (tiles == s_surfaceUvTiles.end() || tiles->second);
    const pbr::Edit& ed = pbrMaps->edit;
    const bool pomOn = ed.present ? ed.pomEnabled : pbrMaps->pomDefault;
    const float metresPerUv = mpu != s_surfaceMetresPerUv.end() ? mpu->second : 0.0f;
    float outUv = 0.0f, inUv = 0.0f;
    if (metresPerUv > 0.0f) {
      const float autoUv = std::min(pbrMaps->reliefUv > 0.0f ? pbrMaps->reliefUv : pbrMaps->heightMetres / metresPerUv,
                                    0.04f);
      // outward: the editor's depth (cm) when set, else the automatic one, plus older edits' outward offset; inward:
      // the editor's (default 0: the relief rises above the surface). Remix puts the polygon at
      // neutralHeight = displaceIn / (displaceIn + displaceOut) of the height map.
      outUv = (ed.pomDepthCm >= 0.0f ? ed.pomDepthCm * 0.01f / metresPerUv : autoUv) +
              std::max(ed.pomOutCm, 0.0f) * 0.01f / metresPerUv;
      inUv = std::max(ed.pomInCm, 0.0f) * 0.01f / metresPerUv;
      if (!layer && albedoTex && s_textures.count(albedoTex)) {
        s_autoDepthCm[s_textures[albedoTex].contentHash] = autoUv * metresPerUv * 100.0f;
      }
    }
    if (pbrMaps->height && pomAllowed && pomOn && outUv + inUv > 0.0f) {
      heightPath = texturePath(pbrMaps->height);
      opaque.heightTexture = heightPath.c_str();
      opaque.displaceIn = inUv;
      opaque.displaceOut = outUv;
    } else if (layer && pbrMaps->height) {
      // Terrain layer overlay without POM: the height map with no displacement (Remix keeps the texture index but
      // not the POM flag), read by the fork's height blend (key L): opacity = saturate((h + w (1 + k) - 1) / k).
      heightPath = texturePath(pbrMaps->height);
      opaque.heightTexture = heightPath.c_str();
      opaque.displaceIn = 0.0f;
      opaque.displaceOut = 0.0f;
    }
    static std::set<std::pair<uint64_t, uint8_t>> s_logged;
    const uint64_t content = albedoTex && s_textures.count(albedoTex) ? s_textures[albedoTex].contentHash : 0;
    if (s_logged.insert({ content, layer }).second) {
      log::line("pbr: material %016llX layer %u blend %u metres/uv %.3f tiles %d relief/uv %.3f pom %d%d -> "
                "displaceOut %.5f In %.5f", static_cast<unsigned long long>(content), layer, blend,
                mpu != s_surfaceMetresPerUv.end() ? mpu->second : -1.0f,
                tiles == s_surfaceUvTiles.end() ? -1 : int(tiles->second), pbrMaps->reliefUv, pomAllowed ? 1 : 0,
                pomOn ? 1 : 0, opaque.displaceOut, opaque.displaceIn);
    }
  }
  opaque.thinFilmThickness_value = 200.0f;
  // Engine alpha test (material flags bit 24, ALPHAREF, ALPHAFUNC) -> Remix AlphaTestType (6 GreaterOrEqual,
  // 1 Less, 7 Always).
  opaque.alphaTestType = (m && m->alphaTest && !layer) ? (m->alphaLess ? 1 : 6) : 7;
  if ((m && m->hiddenEffect && !layer) ||
      (!layer && blend == game::kBlendAlpha && albedoTex && isShadowDecal(albedoTex))) {
    opaque.alphaTestType = 0;  // AlphaTestType::kNever: invisible, casts nothing
  }
  opaque.alphaReferenceValue = m ? m->alphaRef : 0;
  // Engine blend (FUN_00A21350) -> the BlendType Remix itself assigns to the same D3D factors
  // (rtx_instance_manager.cpp:738-741 SRCALPHA/INVSRCALPHA -> kAlpha, 810-815 DESTCOLOR/ZERO -> kMultiplicative).
  if (blend == game::kBlendAlpha || blend == game::kBlendMultiply) {
    opaque.blendType_hasvalue = 1;
    opaque.blendType_value = blend == game::kBlendAlpha ? 0 /* kAlpha */ : 7 /* kMultiplicative */;
  }
  remixapi_MaterialInfo material = {};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = hash;
  if (albedoPath.empty() && m && m->constantColor && !layer) {
    opaque.albedoConstant = { m->color[0], m->color[1], m->color[2] };
  } else if (albedoPath.empty() && !layer && !(m && m->water)) {
    // diagnostics (user 2026-10-03: "NPCs sometimes have grey parts"): which PS left a surface without colour
    static std::set<uint64_t> s_reported;
    const uint64_t ps = m ? m->shaderHash : 0;
    if (s_reported.insert(ps).second) {
      log::line("material %016llX without albedo: PS %016llX skin %d skinned/dynamic %d (Remix grey constant)",
                static_cast<unsigned long long>(hash), static_cast<unsigned long long>(ps), m && m->skin ? 1 : 0,
                s_skinnedSurfaces.count(hash) ? 1 : 0);
    }
  }
  material.albedoTexture = albedoPath.empty() ? nullptr : albedoPath.c_str();
  material.normalTexture = normalPath.empty() ? nullptr : normalPath.c_str();
  ++s_materialsCreated;
  material.spriteSheetRow = 1;
  material.spriteSheetCol = 1;
  material.filterMode = 1;
  material.wrapModeU = 1;
  material.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  if (s_api.CreateMaterial(&material, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    return nullptr;
  }
  s_surfaceMaterials[hash] = handle;
  return handle;
}

remixapi_MaterialHandle surfaceMaterial(uint64_t meshKey, const void* staticMesh, uint32_t range,
                                        const void* matInst, uint8_t blend = 0, uint8_t layer = 0,
                                        bool skinned = false) {
  const uint64_t hash = surfaceMaterialHash(meshKey, range, matInst, layer);
  if (blend) {
    s_surfaceBlend[hash] = blend;
  }
  if (skinned) {
    s_skinnedSurfaces.insert(hash);
  }
  const auto mpu = s_metresPerUv.find({ staticMesh, range });
  if (mpu != s_metresPerUv.end()) {
    s_surfaceMetresPerUv[hash] = mpu->second;
  }
  const auto tiles = s_uvTiles.find({ staticMesh, range });
  if (tiles != s_uvTiles.end()) {
    s_surfaceUvTiles[hash] = tiles->second;
  }
  if (layer) {
    s_surfaceLayer[hash] = layer;
  }
  auto existing = s_surfaceMaterials.find(hash);
  if (existing != s_surfaceMaterials.end()) {
    return existing->second;
  }
  auto state = s_surfaceState.find({ staticMesh, range, matInst });
  if (layer == 0) {
    s_meshRanges[meshKey].push_back({ range, matInst });
  }
  return createSurfaceMaterial(hash, state != s_surfaceState.end() ? &state->second : nullptr);
}

void applyMaterialUpdates() {
  for (auto& u : materials::takeUpdates()) {
    s_surfaceState[{ u.staticMesh, u.range, u.matInst }] = u.material;
    if (const auto pending = s_vegPending.find({ u.staticMesh, u.range }); pending != s_vegPending.end() && !u.matInst) {
      if (u.material.alphaTest) {
        const VegPending& p = pending->second;
        const uint64_t id = vegGeometryId(p.vertices, p.indices);
        vegExport(id, "static", p.vertices, p.indices, &u.material);
        vegRecordRange(p.meshKey, u.range, id);
      }
      s_vegPending.erase(pending);
    }
    auto key = s_meshKeyOf.find(u.staticMesh);
    if (key != s_meshKeyOf.end()) {
      for (uint8_t layer = 0; layer <= 2; ++layer) {
        const uint64_t hash = surfaceMaterialHash(key->second, u.range, u.matInst, layer);
        auto existing = s_surfaceMaterials.find(hash);
        if (existing != s_surfaceMaterials.end()) {
          // Same hash: the mesh surfaces keep resolving to it.
          s_api.DestroyMaterial(existing->second);
          s_surfaceMaterials.erase(existing);
          createSurfaceMaterial(hash, &u.material);
        }
      }
    }
    materials::releaseTextures(u.material);  // references carried by the update
  }
}

// ---- Live material edits (Remix runtime material editor, key M). The editor writes <hash>_edit.json; every half
// second the loaded PBR sets are checked for a newer edit file, the maps are rebuilt on a worker thread (decode,
// adjust, re-encode: seconds for a 4096 set), then uploaded here; maps the edit did not change keep their textures.
// The materials using that albedo are re-created with the same hashes. Permanent inversions (<hash>_bake.txt) are
// written to the map files by the same worker (pbr::bakeRequests), then those maps are re-uploaded.

struct PbrEditResult {
  uint64_t content = 0;
  uint64_t stamp = 0;
  pbr::Edit edit;
  std::unique_ptr<pbr::Override> maps;
  uint32_t baked = 0;  // maps inverted permanently (1 << pbr::Map)
};
std::mutex s_pbrEditMutex;
std::vector<PbrEditResult> s_pbrEditResults;

bool sameMapEdit(const pbr::Edit& a, const pbr::Edit& b, int m) {
  const pbr::MapEdit &x = a.map[m], &y = b.map[m];
  bool same = x.enabled == y.enabled && x.brightness == y.brightness && x.contrast == y.contrast &&
              x.blur == y.blur && x.invert == y.invert;
  if (m == pbr::kAlbedo) {
    same = same && a.albedoSaturation == b.albedoSaturation && a.tint[0] == b.tint[0] && a.tint[1] == b.tint[1] &&
           a.tint[2] == b.tint[2];
  } else if (m == pbr::kNormal) {
    same = same && a.normalStrength == b.normalStrength;
  }
  return same;
}

void refreshPbrMaterials(uint64_t content) {
  for (auto& [key, m] : s_surfaceState) {
    bool uses = false;
    for (IDirect3DTexture9* tex : { m.albedo, m.layerAlbedo[0], m.layerAlbedo[1] }) {
      const auto t = tex ? s_textures.find(tex) : s_textures.end();
      uses = uses || (t != s_textures.end() && t->second.contentHash == content);
    }
    const auto meshKey = s_meshKeyOf.find(std::get<0>(key));
    if (!uses || meshKey == s_meshKeyOf.end()) {
      continue;
    }
    for (uint8_t layer = 0; layer <= 2; ++layer) {
      const uint64_t hash = surfaceMaterialHash(meshKey->second, std::get<1>(key), std::get<2>(key), layer);
      auto existing = s_surfaceMaterials.find(hash);
      if (existing != s_surfaceMaterials.end()) {
        s_api.DestroyMaterial(existing->second);
        s_surfaceMaterials.erase(existing);
        createSurfaceMaterial(hash, &m);
      }
    }
  }
}

void pollPbrEdits() {
  static uint32_t s_counter = 0;
  if (!pbr::enabled() || ++s_counter % 30 != 0) {
    return;
  }
  // finished rebuilds
  std::vector<PbrEditResult> done;
  {
    std::lock_guard lock { s_pbrEditMutex };
    done.swap(s_pbrEditResults);
  }
  for (auto& r : done) {
    auto it = s_pbrTextures.find(r.content);
    if (it == s_pbrTextures.end()) {
      continue;
    }
    PbrTextures& t = it->second;
    PbrTextures old = t;
    uint64_t* slot[pbr::kMapCount] = { &t.albedo, &t.normal, &t.roughness, &t.metallic, &t.height };
    std::vector<uint64_t> retired;
    for (int m = 0; m < pbr::kMapCount; ++m) {
      if (r.maps && (!sameMapEdit(old.edit, r.edit, m) || (r.baked >> m & 1))) {
        retired.push_back(*slot[m]);
        *slot[m] = uploadPackedPbr(r.maps->packed[m]);
      }
    }
    t.edit = r.edit;
    t.editStamp = r.stamp;
    t.pending = false;
    t.valid = t.albedo != 0 && t.edit.enabled;
    refreshPbrMaterials(r.content);
    for (uint64_t h : retired) {
      destroyPbrTexture(h);
    }
    writeLiveTextures();
    log::line("pbr: edit applied to %016llX (%zu maps rebuilt)", static_cast<unsigned long long>(r.content),
              retired.size());
  }
  // new edits
  for (auto& [content, t] : s_pbrTextures) {
    if (t.pending) {
      continue;
    }
    const uint64_t stamp = pbr::editStamp(content);
    const bool bake = pbr::bakePending(content);
    if (stamp == t.editStamp && !bake) {
      continue;
    }
    t.pending = true;
    const pbr::Edit previous = t.edit;
    std::thread([content, stamp, previous, bake] {
      PbrEditResult r;
      r.content = content;
      r.stamp = stamp;
      r.baked = bake ? pbr::bakeRequests(content) : 0;
      r.edit = pbr::readEdit(content);
      if (!r.edit.present) {
        r.edit = pbr::Edit {};  // file deleted: back to the workflow's maps
      }
      bool changed = r.baked != 0;
      for (int m = 0; m < pbr::kMapCount; ++m) {
        changed = changed || !sameMapEdit(previous, r.edit, m);
      }
      if (changed) {
        r.maps = pbr::load(content);
        if (r.maps) {
          pbr::Edit applied = r.edit;
          applied.present = true;
          pbr::applyEdit(*r.maps, applied);
        }
      }
      std::lock_guard lock { s_pbrEditMutex };
      s_pbrEditResults.push_back(std::move(r));
    }).detach();
  }
}

void forgetMeshMaterials(uint64_t meshKey) {
  const void* staticMesh = reinterpret_cast<const void*>(static_cast<uintptr_t>(meshKey & 0xFFFFFFFFull));
  auto ranges = s_meshRanges.find(meshKey);
  if (ranges != s_meshRanges.end()) {
    for (const auto& [range, matInst] : ranges->second) {
      for (uint8_t layer = 0; layer <= 2; ++layer) {
        const uint64_t hash = surfaceMaterialHash(meshKey, range, matInst, layer);
        s_surfaceBlend.erase(hash);
        s_surfaceLayer.erase(hash);
        auto m = s_surfaceMaterials.find(hash);
        if (m != s_surfaceMaterials.end()) {
          s_api.DestroyMaterial(m->second);
          s_surfaceMaterials.erase(m);
        }
      }
      s_surfaceState.erase({ staticMesh, range, matInst });
    }
    s_meshRanges.erase(ranges);
  }
  auto live = s_meshKeyOf.find(staticMesh);
  if (live != s_meshKeyOf.end() && live->second == meshKey) {
    s_meshKeyOf.erase(live);
  }
  for (auto it = s_uvTiles.lower_bound({ staticMesh, 0u }); it != s_uvTiles.end() && it->first.first == staticMesh;) {
    it = s_uvTiles.erase(it);
  }
  for (auto it = s_metresPerUv.lower_bound({ staticMesh, 0u });
       it != s_metresPerUv.end() && it->first.first == staticMesh;) {
    it = s_metresPerUv.erase(it);
  }
  materials::forgetMesh(staticMesh);
}

remixapi_MeshHandle createRemixMesh(const static_geometry::Mesh& mesh, const StaticMeshPart& part, uint64_t hash) {
  std::vector<const static_geometry::Surface*> selected;
  for (const auto& surface : mesh.surfaces) {
    if (surface.doubleSided == part.doubleSided && surface.cell == part.cell && surface.blend == part.blend &&
        surface.decal == part.decal && surface.layer == part.layer && surface.perRange == part.perRange &&
        (!part.perRange || surface.range == part.range)) {
      selected.push_back(&surface);
    }
  }
  if (selected.empty()) {
    return nullptr;
  }
  std::vector<std::vector<remixapi_HardcodedVertex>> vertexStorage(selected.size());
  std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces(selected.size());
  for (size_t s = 0; s < selected.size(); ++s) {
    const auto& src = *selected[s];
    auto& verts = vertexStorage[s];
    verts.resize(src.vertices.size());
    for (size_t v = 0; v < src.vertices.size(); ++v) {
      auto& dst = verts[v];
      dst = {};
      std::memcpy(dst.position, src.vertices[v].position, sizeof(dst.position));
      std::memcpy(dst.normal, src.vertices[v].normal, sizeof(dst.normal));
      std::memcpy(dst.texcoord, src.vertices[v].texcoord, sizeof(dst.texcoord));
      dst.color = src.vertices[v].color;  // D3DCOLOR bytes = VK_FORMAT_B8G8R8A8_UNORM (rtx_remix_api.cpp:1206)
      if (src.layer) {
        // Terrain layer overlay: rgb = COLOR0 (multiplies every layer), alpha = the layer weight (COLOR1.x / .y).
        dst.color = (dst.color & 0x00FFFFFFu) | (uint32_t(src.vertices[v].layerWeight[src.layer - 1]) << 24);
      }
    }
    auto& surface = surfaces[s];
    surface = {};
    surface.vertices_values = verts.data();
    surface.vertices_count = verts.size();
    surface.indices_values = src.indices.data();
    surface.indices_count = src.indices.size();
    ensureMetresPerUv(staticMeshOf(mesh.key), src.range, src.vertices, src.indices);
    if (!src.layer) {
      vegStaticRange(mesh.key, src);
    }
    surface.material = surfaceMaterial(mesh.key, staticMeshOf(mesh.key), src.range, nullptr, src.blend, src.layer);
    if (!surface.material) {
      surface.material = s_staticMaterial;
    }
  }
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = hash;
  info.surfaces_values = surfaces.data();
  info.surfaces_count = static_cast<uint32_t>(surfaces.size());
  remixapi_MeshHandle handle = nullptr;
  if (s_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    return nullptr;
  }
  return handle;
}

void collisionAdd(const static_geometry::Mesh& mesh);  // vegetation editor ground (below)

void createStaticMesh(const static_geometry::Mesh& mesh) {
  s_meshKeyOf[staticMeshOf(mesh.key)] = mesh.key;
  collisionAdd(mesh);
  std::vector<StaticMeshPart> parts;
  std::vector<StaticMeshPart> groups;
  for (const auto& surface : mesh.surfaces) {
    const StaticMeshPart group { nullptr, surface.doubleSided, surface.cell, surface.blend, surface.decal,
                                 surface.layer, surface.perRange, surface.perRange ? surface.range : 0 };
    if (std::none_of(groups.begin(), groups.end(), [&](const StaticMeshPart& g) {
          return g.doubleSided == group.doubleSided && g.cell == group.cell && g.blend == group.blend &&
                 g.decal == group.decal && g.layer == group.layer && g.perRange == group.perRange &&
                 g.range == group.range;
        })) {
      groups.push_back(group);
    }
  }
  // Base parts first, then layer 1, then layer 2: the overlays are drawn in the engine's blend order.
  std::stable_sort(groups.begin(), groups.end(),
                   [](const StaticMeshPart& a, const StaticMeshPart& b) { return a.layer < b.layer; });
  for (auto group : groups) {
    uint64_t hash = mesh.key ^ (group.doubleSided ? kDoubleSidedHashSalt : 0);
    if (group.cell != static_geometry::Surface::kNoCell) {
      hash ^= (uint64_t(group.cell) + 1) * 0x9E3779B97F4A7C15ull;
    }
    hash ^= (uint64_t(group.blend) * 2 + (group.decal ? 1 : 0)) * 0xC2B2AE3D27D4EB4Full;
    if (group.perRange) {
      hash ^= ((uint64_t(group.range) + 1) * 4 + group.layer) * 0x94D049BB133111EBull;
    }
    for (const auto& surface : mesh.surfaces) {
      if (surface.doubleSided == group.doubleSided && surface.cell == group.cell && surface.blend == group.blend &&
          surface.decal == group.decal && surface.layer == group.layer && surface.perRange == group.perRange &&
          (!group.perRange || surface.range == group.range)) {
        for (const auto& v : surface.vertices) {
          group.vertexAlphaOne = group.vertexAlphaOne && (v.color >> 24) == 0xFF;
        }
        if (group.blend && group.perRange && !group.vertices) {
          auto verts = std::make_shared<std::vector<remixapi_HardcodedVertex>>(surface.vertices.size());
          for (size_t v = 0; v < surface.vertices.size(); ++v) {
            auto& dst = (*verts)[v];
            dst = {};
            std::memcpy(dst.position, surface.vertices[v].position, sizeof(dst.position));
            std::memcpy(dst.normal, surface.vertices[v].normal, sizeof(dst.normal));
            std::memcpy(dst.texcoord, surface.vertices[v].texcoord, sizeof(dst.texcoord));
            dst.color = surface.vertices[v].color;
          }
          group.vertices = verts;
          group.indices = std::make_shared<std::vector<uint32_t>>(surface.indices.begin(), surface.indices.end());
        }
      }
    }
    if ((group.handle = createRemixMesh(mesh, group, hash)) != nullptr) {
      parts.push_back(group);
    }
  }
  if (!parts.empty()) {
    s_staticMeshes[mesh.key] = std::move(parts);
  }
}

// Diagnostic toggle (F8): glass surfaces not submitted.
bool s_hideGlass = false;

// Water meshes of the previous frame (re-created every frame with that frame's texcoords, see drawStatic).
std::vector<remixapi_MeshHandle> s_waterMeshes;
uint64_t s_waterSerial = 0;

uint32_t tintToD3DColor(const float rgb[3]) {
  auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return 0xFF000000u | (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
}

void drawStatic(uint64_t meshKey, const StaticMeshPart& part, const remixapi_Transform& transform) {
  if (!part.handle) {
    return;
  }
  if (const auto veg = s_vegOfMesh.find(meshKey); veg != s_vegOfMesh.end()) {
    for (uint64_t id : veg->second) vegPlace(id, transform);
  }
  if (vegMeshRemoved(meshKey)) {
    return;
  }
  const materials::SurfaceMaterial* state = nullptr;
  if (part.perRange) {
    auto it = s_surfaceState.find({ staticMeshOf(meshKey), part.range, nullptr });
    state = it != s_surfaceState.end() ? &it->second : nullptr;
  }
  const bool layered = state && state->layered;
  if (part.layer && !layered) {
    return;  // weights exist but the range's shader is not the layered terrain PS: nothing to overlay
  }
  if (part.perRange && materials::isRenderTargetSurface(staticMeshOf(meshKey), part.range)) {
    return;  // shows another view's output: rasterized over the ray-traced image instead (hud.cpp)
  }
  if (part.blend == game::kBlendGlass && !(state && state->glass)) {
    return;  // mode 5 is sent only once its PS is known to be the glass graph (translucent material)
  }
  if (s_hideGlass && part.blend == game::kBlendGlass) {
    return;
  }
  float panner[2][3];
  if (state && state->water && part.vertices && part.indices &&
      materials::waterPanner(staticMeshOf(meshKey), part.range, panner)) {
    // Water PS: normal-map uv = Panner01 (c128/c129, animated by the game every frame) x (world.x, world.y, 1),
    // world = the VS's texcoord1 output (pos x g_World, VS 3F07D8C7). The mesh has no texcoord of its own, so this
    // frame's geometry gets those texcoords (world = instance transform x local position).
    std::vector<remixapi_HardcodedVertex> verts = *part.vertices;
    for (auto& v : verts) {
      const float* p = v.position;
      const float wx = transform.matrix[0][0] * p[0] + transform.matrix[0][1] * p[1] + transform.matrix[0][2] * p[2] +
                       transform.matrix[0][3];
      const float wy = transform.matrix[1][0] * p[0] + transform.matrix[1][1] * p[1] + transform.matrix[1][2] * p[2] +
                       transform.matrix[1][3];
      v.texcoord[0] = panner[0][0] * wx + panner[0][1] * wy + panner[0][2];
      v.texcoord[1] = panner[1][0] * wx + panner[1][1] * wy + panner[1][2];
    }
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = verts.data();
    surface.vertices_count = verts.size();
    surface.indices_values = part.indices->data();
    surface.indices_count = part.indices->size();
    surface.material = surfaceMaterial(meshKey, staticMeshOf(meshKey), part.range, nullptr, part.blend, 0);
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0xAC1A000000000000ull | (++s_waterSerial & 0x0000FFFFFFFFFFFFull);
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle handle = nullptr;
    if (s_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
      return;
    }
    s_waterMeshes.push_back(handle);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = handle;
    inst.transform = transform;
    inst.doubleSided = part.doubleSided ? 1 : 0;
    drawInstance(&inst);
    return;
  }
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.mesh = part.handle;
  inst.transform = transform;
  inst.doubleSided = part.doubleSided ? 1 : 0;
  remixapi_InstanceInfoBlendEXT blend = {};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.tFactor = 0xFFFFFFFF;
  blend.writeMask = 0xF;
  blend.isVertexColorBakedLighting = 0;
  if (part.blend && part.blend != game::kBlendGlass && !(state && state->water)) {
    // The blended PS multiply albedo by the vertex colour rgb and opacity by its alpha (PS 0x22E78CC0 / 0x22E7CC80;
    // their alpha constants Alphablend_5 / Operator7_8 are 1 in every Masyaf draw). Remix's defaults ignore the
    // vertex colour (rtx_materials.h:348-353), so the texture stage ops are given per instance; blending itself
    // comes from the material (useDrawCallAlphaState = false).
    blend.textureColorArg1Source = 1;  // RtTextureArgSource::Texture
    blend.textureColorArg2Source = 2;  // RtTextureArgSource::VertexColor0
    blend.textureColorOperation = 3;   // DxvkRtTextureOperation::Modulate
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 2;
    blend.textureAlphaOperation = 3;
    if (state && state->blendTinted) {
      // Tinted blended PS (game.h kBlendTintPs): rgb factor through tFactor. With COLOR0: Texture x VertexColor0,
      // then x tFactor (isTextureFactorBlend, opaque_surface_material_interaction.slangh:720). Without: Texture x
      // TFactor and alpha = texture alpha.
      blend.tFactor = (tintToD3DColor(state->blendColor) & 0x00FFFFFFu) |
                      (static_cast<uint32_t>(std::lround(std::clamp(state->blendAlpha, 0.0f, 1.0f) * 255.0f)) << 24);
      if (state->blendVertexColor) {
        blend.isTextureFactorBlend = 1;
        if (part.vertexAlphaOne) {
          blend.textureAlphaArg2Source = 3;  // COLOR0.a = 1 on this part: alpha = texture alpha x tFactor alpha
        }
      } else {
        blend.textureColorArg2Source = 3;  // RtTextureArgSource::TFactor
        blend.textureAlphaArg2Source = 3;
        blend.textureAlphaOperation = 1;   // SelectArg1: the PS alpha is the texture alpha
      }
    }
    inst.pNext = &blend;
    if (part.decal) {
      inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
    }
  } else if (layered) {
    // Layered terrain PS (game.h kLayeredTerrainPs): each layer = tex * COLOR0.rgb * its tint (tFactor, applied by
    // isTextureFactorBlend, opaque_surface_material_interaction.slangh:720); base opaque, layers 1/2 overlaid with
    // opacity = their vertex weight (alpha op SelectArg2 = VertexColor0), which composes to the PS's
    // lerp(lerp(base, layer1, w1), layer2, w2). Overlays are coplanar decals.
    blend.textureColorArg1Source = 1;
    blend.textureColorArg2Source = 2;
    blend.textureColorOperation = 3;
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 2;
    blend.textureAlphaOperation = part.layer ? 2 /* SelectArg2 */ : 1 /* SelectArg1 */;
    blend.tFactor = tintToD3DColor(state->tint[part.layer]);
    blend.isTextureFactorBlend = 1;
    inst.pNext = &blend;
    if (part.layer) {
      inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
    }
  }
  drawInstance(&inst);
}
// Engine world matrices are row-vector (p' = p * W); remixapi_Transform is a column-vector 3x4.
remixapi_Transform toRemixTransform(const game::Mat4& w) {
  remixapi_Transform t;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      t.matrix[r][c] = w.m[c * 4 + r];
    }
  }
  return t;
}

// ---- Skinned characters: one Remix mesh per (engine mesh, range, MaterialInstance) with 4 bones per vertex (the
// material is bound per mesh surface, and characters sharing a mesh draw it with different MaterialInstances);
// every frame each colour-pass draw becomes an instance with identity transform (the engine's skinned world is
// identity) and the engine's final bone matrices chained as remixapi_InstanceInfoBoneTransformsEXT
// (docs/notes/skinned.md).

// Unique placements per light-channel bit, world (static + masked) and skinned (see worldChannels()).
uint64_t s_worldPlacements[8] = {}, s_skinnedPlacements[8] = {};

using SkinnedKey = std::tuple<uint64_t, uint32_t, uint8_t, const void*>;  // (mesh key, range, cull, matInst)
using GeometryKey = std::tuple<uint64_t, uint32_t, uint8_t>;               // (mesh key, range, cull)
std::map<GeometryKey, skinned::RangeMesh> s_skinnedGeometry;  // decoded ranges, kept for new matInsts
std::map<SkinnedKey, remixapi_MeshHandle> s_skinnedMeshes;

remixapi_MeshHandle createSkinnedMesh(const skinned::RangeMesh& range, const void* matInst) {
  std::vector<remixapi_HardcodedVertex> verts(range.vertices.size());
  std::vector<float> weights(range.vertices.size() * 4);
  std::vector<uint32_t> bones(range.vertices.size() * 4);
  for (size_t v = 0; v < range.vertices.size(); ++v) {
    const auto& src = range.vertices[v];
    auto& dst = verts[v];
    dst = {};
    std::memcpy(dst.position, src.position, sizeof(dst.position));
    std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
    std::memcpy(dst.texcoord, src.texcoord, sizeof(dst.texcoord));
    dst.color = 0xFFFFFFFF;
    for (int i = 0; i < 4; ++i) {
      weights[v * 4 + i] = src.weights[i];
      bones[v * 4 + i] = src.bones[i];
    }
  }
  s_meshKeyOf[staticMeshOf(range.meshKey)] = range.meshKey;
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = verts.data();
  surface.vertices_count = verts.size();
  surface.indices_values = range.indices.data();
  surface.indices_count = range.indices.size();
  surface.skinning_hasvalue = 1;
  surface.skinning_value.bonesPerVertex = 4;
  surface.skinning_value.blendWeights_values = weights.data();
  surface.skinning_value.blendWeights_count = static_cast<uint32_t>(weights.size());
  surface.skinning_value.blendIndices_values = bones.data();
  surface.skinning_value.blendIndices_count = static_cast<uint32_t>(bones.size());
  ensureMetresPerUv(staticMeshOf(range.meshKey), range.range, range.vertices, range.indices);
  surface.material = surfaceMaterial(range.meshKey, staticMeshOf(range.meshKey), range.range, matInst, 0, 0, true);
  if (!surface.material) {
    surface.material = s_staticMaterial;
  }
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = (range.meshKey * 0x9E3779B97F4A7C15ull) ^ (uint64_t(range.range) + 1) * 0xBF58476D1CE4E5B9ull ^
              (uint64_t(reinterpret_cast<uintptr_t>(matInst)) + 1) * 0x94D049BB133111EBull ^
              (uint64_t(range.cull) + 1) * 0xD6E8FEB86659FD93ull;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  const remixapi_ErrorCode err = s_api.CreateMesh(&info, &handle);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
    log::line("CreateMesh(skinned) %016llx/%u/%p -> %d", static_cast<unsigned long long>(range.meshKey), range.range,
              matInst, err);
    return nullptr;
  }
  return handle;
}

void submitSkinned(const std::vector<uint64_t>& destroyedMeshes) {
  skinned::Frame frame = skinned::takeFrame();
  for (int bit = 0; bit < 8; ++bit) {
    s_skinnedPlacements[bit] += frame.channelPlacements[bit];
  }
  for (uint64_t key : destroyedMeshes) {
    for (auto it = s_skinnedMeshes.lower_bound({ key, 0u, uint8_t(0), nullptr });
         it != s_skinnedMeshes.end() && std::get<0>(it->first) == key;) {
      if (it->second) {
        s_api.DestroyMesh(it->second);
      }
      it = s_skinnedMeshes.erase(it);
    }
    for (auto it = s_skinnedGeometry.lower_bound({ key, 0u, uint8_t(0) });
         it != s_skinnedGeometry.end() && std::get<0>(it->first) == key;) {
      it = s_skinnedGeometry.erase(it);
    }
  }
  for (auto& range : frame.newRanges) {
    const GeometryKey key { range.meshKey, range.range, range.cull };
    s_skinnedGeometry[key] = std::move(range);
  }
  const remixapi_Transform identity = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
  size_t created = 0;
  for (const auto& draw : frame.draws) {
    const SkinnedKey key { draw.meshKey, draw.range, draw.cull, draw.matInst };
    const GeometryKey geometryKey { draw.meshKey, draw.range, draw.cull };
    auto it = s_skinnedMeshes.find(key);
    if (it == s_skinnedMeshes.end()) {
      auto geometry = s_skinnedGeometry.find(geometryKey);
      if (geometry == s_skinnedGeometry.end()) {
        continue;  // range rejected by the decoder
      }
      it = s_skinnedMeshes.emplace(key, createSkinnedMesh(geometry->second, draw.matInst)).first;
      ++created;
    }
    if (!it->second) {
      continue;
    }
    static_assert(sizeof(remixapi_Transform) == 12 * sizeof(float));
    remixapi_InstanceInfoBoneTransformsEXT bones = {};
    bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    bones.boneTransforms_values = reinterpret_cast<const remixapi_Transform*>(draw.bones.data());
    bones.boneTransforms_count = static_cast<uint32_t>(draw.bones.size() / 12);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.pNext = &bones;
    inst.mesh = it->second;
    inst.transform = identity;
    inst.doubleSided = draw.cull == 1 ? 1 : 0;  // engine D3DCULL_NONE
    drawInstance(&inst);
  }
}
// ---- Cloth (DX9DynamicSubMeshInstance CPU-vertex path): the vertices change every frame, so each draw becomes a
// new Remix mesh for this frame; the previous frame's meshes are destroyed first.
// Clutter (instanced path): the geometry is fixed, so one Remix mesh per (geometry, MaterialInstance, sidedness) is
// kept and drawn once per engine instance transform every frame.
std::vector<remixapi_MeshHandle> s_dynamicMeshes;
uint64_t s_dynamicSerial = 0;

// ---- Rebuilt vegetation (user 2026-10-03: "make the grass worthy of a 2026 game"): an exported mesh id with a folder
// <pbr>\vegetation\replace\<id>\ (mesh.obj in the export's local space, albedo / normal / roughness .ac1t from
// tools/veg_replace_pack.py) is drawn with that mesh and material instead, at the same engine transforms. Alpha-tested
// (GreaterOrEqual 128, the blades' cut-out), double-sided. Built once, kept for the session.
std::unordered_map<uint64_t, remixapi_MeshHandle> s_vegReplaced;

// OBJ (v / vt / vn, faces "a/b/c", n-gons fanned): one Remix vertex per distinct (v, vt, vn).
bool vegLoadObj(const std::wstring& path, std::vector<remixapi_HardcodedVertex>& verts, std::vector<uint32_t>& indices) {
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) {
    return false;
  }
  std::vector<std::array<float, 3>> pos, nrm;
  std::vector<std::array<float, 2>> uv;
  std::map<std::tuple<int, int, int>, uint32_t> remap;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    float x = 0, y = 0, z = 0;
    if (line[0] == 'v' && line[1] == ' ' && sscanf_s(line + 2, "%f %f %f", &x, &y, &z) == 3) {
      pos.push_back({ x, y, z });
    } else if (line[0] == 'v' && line[1] == 't' && sscanf_s(line + 3, "%f %f", &x, &y) == 2) {
      uv.push_back({ x, 1.0f - y });  // OBJ v up, Remix v down (the export flipped it)
    } else if (line[0] == 'v' && line[1] == 'n' && sscanf_s(line + 3, "%f %f %f", &x, &y, &z) == 3) {
      nrm.push_back({ x, y, z });
    } else if (line[0] == 'f' && line[1] == ' ') {
      std::vector<uint32_t> poly;
      char* ctx = nullptr;
      for (char* tok = strtok_s(line + 2, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx)) {
        int vi = 0, ti = 0, ni = 0;
        if (strstr(tok, "//")) {
          sscanf_s(tok, "%d//%d", &vi, &ni);
        } else if (sscanf_s(tok, "%d/%d/%d", &vi, &ti, &ni) < 1) {
          continue;
        }
        const auto key = std::make_tuple(vi, ti, ni);
        auto it = remap.find(key);
        if (it == remap.end()) {
          remixapi_HardcodedVertex v = {};
          if (vi > 0 && size_t(vi) <= pos.size()) std::memcpy(v.position, pos[vi - 1].data(), sizeof(v.position));
          if (ti > 0 && size_t(ti) <= uv.size()) std::memcpy(v.texcoord, uv[ti - 1].data(), sizeof(v.texcoord));
          if (ni > 0 && size_t(ni) <= nrm.size()) std::memcpy(v.normal, nrm[ni - 1].data(), sizeof(v.normal));
          v.color = 0xFFFFFFFF;
          it = remap.emplace(key, uint32_t(verts.size())).first;
          verts.push_back(v);
        }
        poly.push_back(it->second);
      }
      for (size_t k = 1; k + 1 < poly.size(); ++k) {
        indices.insert(indices.end(), { poly[0], poly[k], poly[k + 1] });
      }
    }
  }
  fclose(f);
  return !verts.empty();
}

// Rebuilt vegetation material: albedo / normal / roughness .ac1t of a folder, alpha-tested (GreaterOrEqual 128, the
// cards' cut-out).
remixapi_MaterialHandle vegLoadMaterial(const std::wstring& dir, uint64_t hash) {
  pbr::Packed albedo, normal, roughness;
  pbr::loadPackedFile(dir + L"albedo.ac1t", albedo);
  pbr::loadPackedFile(dir + L"normal.ac1t", normal);
  pbr::loadPackedFile(dir + L"roughness.ac1t", roughness);
  const uint64_t albedoTex = uploadPackedPbr(albedo), normalTex = uploadPackedPbr(normal),
                 roughTex = uploadPackedPbr(roughness);
  const std::wstring albedoPath = albedoTex ? texturePath(albedoTex) : L"";
  const std::wstring normalPath = normalTex ? texturePath(normalTex) : L"";
  const std::wstring roughPath = roughTex ? texturePath(roughTex) : L"";
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = { 0.5f, 0.5f, 0.5f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 0.8f;
  opaque.roughnessTexture = roughPath.empty() ? nullptr : roughPath.c_str();
  opaque.thinFilmThickness_value = 200.0f;
  opaque.alphaTestType = 6;  // GreaterOrEqual
  opaque.alphaReferenceValue = 128;
  remixapi_MaterialInfo material = {};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = hash;
  material.albedoTexture = albedoPath.empty() ? nullptr : albedoPath.c_str();
  material.normalTexture = normalPath.empty() ? nullptr : normalPath.c_str();
  material.spriteSheetRow = 1;
  material.spriteSheetCol = 1;
  material.filterMode = 1;
  material.wrapModeU = 1;
  material.wrapModeV = 1;
  remixapi_MaterialHandle mat = nullptr;
  if (s_api.CreateMaterial(&material, &mat) != REMIXAPI_ERROR_CODE_SUCCESS) {
    mat = s_staticMaterial;
  }
  log::line("vegetation: material %ls (textures %d%d%d)", dir.c_str(), albedoTex ? 1 : 0, normalTex ? 1 : 0,
            roughTex ? 1 : 0);
  return mat;
}

remixapi_MeshHandle vegCreateMesh(const std::wstring& obj, remixapi_MaterialHandle mat, uint64_t hash) {
  std::vector<remixapi_HardcodedVertex> verts;
  std::vector<uint32_t> indices;
  if (!vegLoadObj(obj, verts, indices)) {
    log::line("vegetation: cannot read %ls", obj.c_str());
    return nullptr;
  }
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = verts.data();
  surface.vertices_count = verts.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  surface.material = mat;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = hash;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  if (s_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    handle = nullptr;
  }
  log::line("vegetation: mesh %ls (%zu vertices, %zu triangles) -> %p", obj.c_str(), verts.size(), indices.size() / 3,
            handle);
  return handle;
}

remixapi_MeshHandle vegReplacementMesh(uint64_t id) {
  if (!pbr::enabled()) {
    return nullptr;
  }
  const auto found = s_vegReplaced.find(id);
  if (found != s_vegReplaced.end()) {
    return found->second;
  }
  s_vegReplaced[id] = nullptr;
  wchar_t name[64];
  swprintf_s(name, L"\\vegetation\\replace\\%016llX\\", static_cast<unsigned long long>(id));
  const std::wstring dir = pbr::folder() + name;
  if (GetFileAttributesW((dir + L"mesh.obj").c_str()) == INVALID_FILE_ATTRIBUTES) {
    return nullptr;
  }
  const remixapi_MaterialHandle mat = vegLoadMaterial(dir, 0xAC1F000000000000ull | (id & 0x0000FFFFFFFFFFFFull));
  const remixapi_MeshHandle handle = vegCreateMesh(dir + L"mesh.obj", mat, 0xAC1E700000000000ull ^ id);
  s_vegReplaced[id] = handle;
  return handle;
}

// ---- Vegetation scatter (user 2026-10-03: "use the places of the original grass and put the plants we made there,
// not 1-1 but in the most natural way, by number and size"). <pbr>\vegetation\scatter\<clutter id>\ from
// tools/veg_scatter_plan.py replaces that clutter mesh:
//   plants.txt  "<folder> <obj>" per mesh index; the folder holds the plant's albedo / normal / roughness .ac1t;
//   plan.txt    "T x y z"                          a grass tuft the engine placed when the level was exported,
//               "P mesh range m00 .. m23"          a planned plant (world 3x4), drawn within `range` metres of the
//                                                  camera while the level the plan was made for is loaded,
//               "F template mesh dx dy yaw scale"  templates for tufts not in the plan (areas not visited when
//                                                  exporting, other levels), placed in the tuft's frame.
// The level is recognised by its tufts: the plan is drawn while the engine places tufts where the plan has one
// (kept for 600 frames, so interiors and views without grass keep it). F7 shows the original grass instead.
struct ScatterPlant {
  uint32_t mesh;
  float range2;
  float m[3][4];
};
struct ScatterTemplatePlant {
  uint32_t mesh;
  float dx, dy, yaw, scale;
};
struct Scatter {
  std::vector<remixapi_MeshHandle> meshes;
  std::vector<ScatterPlant> plants;
  std::unordered_map<int64_t, std::vector<uint32_t>> cells;               // 16 m cells -> plants
  std::unordered_map<int64_t, std::vector<std::array<float, 3>>> tufts;   // 1 m cells -> known tufts
  std::vector<std::vector<ScatterTemplatePlant>> templates;
  float maxRange = 0.0f;
  uint64_t lastMatch = 0;  // frame of the last engine tuft found in the plan
};
std::unordered_map<uint64_t, std::unique_ptr<Scatter>> s_scatters;
uint64_t s_scatterFrame = 0;
bool s_scatterHidden = false;
float s_cameraPos[3] = {};

constexpr float kScatterCell = 16.0f;

int64_t scatterCell(int64_t ix, int64_t iy) {
  return (ix << 32) ^ (iy & 0xFFFFFFFFll);
}

int64_t scatterCell(float x, float y, float size) {
  return scatterCell(int64_t(std::floor(x / size)), int64_t(std::floor(y / size)));
}

Scatter* scatterFor(uint64_t id) {
  if (!pbr::enabled() || s_scatterHidden) {
    return nullptr;
  }
  const auto found = s_scatters.find(id);
  if (found != s_scatters.end()) {
    return found->second.get();
  }
  auto& slot = s_scatters[id];
  wchar_t name[64];
  swprintf_s(name, L"\\vegetation\\scatter\\%016llX\\", static_cast<unsigned long long>(id));
  const std::wstring dir = pbr::folder() + name;
  FILE* f = _wfopen((dir + L"plants.txt").c_str(), L"r");
  if (!f) {
    return nullptr;
  }
  auto sc = std::make_unique<Scatter>();
  std::map<std::string, remixapi_MaterialHandle> materials;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char folder[256] = {}, obj[256] = {};
    if (sscanf_s(line, "%255s %255s", folder, unsigned(sizeof(folder)), obj, unsigned(sizeof(obj))) != 2) {
      continue;
    }
    const std::string key(folder);
    const std::wstring folderDir = dir + std::wstring(key.begin(), key.end()) + L"\\";
    auto mat = materials.find(key);
    if (mat == materials.end()) {
      const uint64_t h = (uint64_t(std::hash<std::string>()(key)) * 0x9E3779B97F4A7C15ull) ^ id;
      mat = materials.emplace(key, vegLoadMaterial(folderDir, 0xAC1F500000000000ull | (h & 0x00000FFFFFFFFFFFull))).first;
    }
    const std::string objName(obj);
    const uint64_t h = (uint64_t(std::hash<std::string>()(key + "/" + objName)) * 0xC2B2AE3D27D4EB4Full) ^ id;
    sc->meshes.push_back(vegCreateMesh(folderDir + std::wstring(objName.begin(), objName.end()), mat->second,
                                       0xAC1E500000000000ull | (h & 0x00000FFFFFFFFFFFull)));
  }
  fclose(f);
  f = _wfopen((dir + L"plan.txt").c_str(), L"r");
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      if (line[0] == 'T') {
        float x, y, z;
        if (sscanf_s(line + 1, "%f %f %f", &x, &y, &z) == 3) {
          sc->tufts[scatterCell(x, y, 1.0f)].push_back({ x, y, z });
        }
      } else if (line[0] == 'P') {
        ScatterPlant p = {};
        float range = 0.0f;
        float* m = &p.m[0][0];
        if (sscanf_s(line + 1, "%u %f %f %f %f %f %f %f %f %f %f %f %f %f", &p.mesh, &range, m, m + 1, m + 2, m + 3,
                     m + 4, m + 5, m + 6, m + 7, m + 8, m + 9, m + 10, m + 11) == 14 &&
            p.mesh < sc->meshes.size()) {
          p.range2 = range * range;
          sc->maxRange = std::max(sc->maxRange, range);
          sc->cells[scatterCell(p.m[0][3], p.m[1][3], kScatterCell)].push_back(uint32_t(sc->plants.size()));
          sc->plants.push_back(p);
        }
      } else if (line[0] == 'F') {
        unsigned t = 0;
        ScatterTemplatePlant p = {};
        if (sscanf_s(line + 1, "%u %u %f %f %f %f", &t, &p.mesh, &p.dx, &p.dy, &p.yaw, &p.scale) == 6 &&
            p.mesh < sc->meshes.size() && t < 4096) {
          if (sc->templates.size() <= t) sc->templates.resize(t + 1);
          sc->templates[t].push_back(p);
        }
      }
    }
    fclose(f);
  }
  log::line("vegetation scatter %016llX: %zu meshes, %zu planned plants, %zu tuft cells, %zu templates",
            static_cast<unsigned long long>(id), sc->meshes.size(), sc->plants.size(), sc->tufts.size(),
            sc->templates.size());
  slot = std::move(sc);
  return slot.get();
}

void drawScatterInstance(remixapi_MeshHandle mesh, const float (&m)[3][4]) {
  if (!mesh) {
    return;
  }
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.mesh = mesh;
  static_assert(sizeof(inst.transform) == sizeof(m));
  std::memcpy(&inst.transform, m, sizeof(inst.transform));
  inst.doubleSided = 1;
  drawInstance(&inst);
}

// Engine tufts of a scatter's clutter mesh: known ones mark the level as the planned one; unknown ones get a template
// (picked by position, so a tuft keeps its plants), offsets in the tuft's frame (on the slope), plants upright with
// a quarter of the slope's tilt.
template <typename Transform>
void scatterTufts(Scatter& sc, const std::vector<Transform>& instances) {
  for (const auto& t : instances) {
    const float x = t.m[0][3], y = t.m[1][3], z = t.m[2][3];
    bool known = false;
    for (int64_t gx = int64_t(std::floor(x)) - 1; gx <= int64_t(std::floor(x)) + 1 && !known; ++gx) {
      for (int64_t gy = int64_t(std::floor(y)) - 1; gy <= int64_t(std::floor(y)) + 1 && !known; ++gy) {
        const auto cell = sc.tufts.find(scatterCell(gx, gy));
        if (cell == sc.tufts.end()) continue;
        for (const auto& p : cell->second) {
          if ((p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) + (p[2] - z) * (p[2] - z) < 0.09f) {
            known = true;
            break;
          }
        }
      }
    }
    if (known) {
      sc.lastMatch = s_scatterFrame;
      continue;
    }
    if (sc.templates.empty()) {
      continue;
    }
    uint64_t h = (uint64_t(int64_t(std::floor(x * 2.0f))) * 0x9E3779B97F4A7C15ull) ^
                 (uint64_t(int64_t(std::floor(y * 2.0f))) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 31;
    const auto& tpl = sc.templates[h % sc.templates.size()];
    float up[3] = { t.m[0][2] * 0.25f, t.m[1][2] * 0.25f, 0.75f + t.m[2][2] * 0.25f };
    const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    for (float& c : up) c /= ul;
    for (const auto& p : tpl) {
      const float ref[3] = { std::cos(p.yaw), std::sin(p.yaw), 0.0f };
      float side[3] = { up[1] * ref[2] - up[2] * ref[1], up[2] * ref[0] - up[0] * ref[2],
                        up[0] * ref[1] - up[1] * ref[0] };
      const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
      for (float& c : side) c /= sl;
      const float fwd[3] = { side[1] * up[2] - side[2] * up[1], side[2] * up[0] - side[0] * up[2],
                             side[0] * up[1] - side[1] * up[0] };
      float m[3][4];
      for (int r = 0; r < 3; ++r) {
        m[r][0] = fwd[r] * p.scale;
        m[r][1] = side[r] * p.scale;
        m[r][2] = up[r] * p.scale;
        m[r][3] = t.m[r][3] + t.m[r][0] * p.dx + t.m[r][1] * p.dy - (r == 2 ? 0.03f : 0.0f);
      }
      drawScatterInstance(sc.meshes[p.mesh], m);
    }
  }
}

bool scatterLevelActive() {
  if (s_scatterHidden) {
    return false;
  }
  for (const auto& [id, sc] : s_scatters) {
    if (sc && sc->lastMatch && s_scatterFrame - sc->lastMatch <= 600) {
      return true;
    }
  }
  return false;
}

void drawScatterPlans() {
  for (auto& [id, sc] : s_scatters) {
    if (!sc || s_scatterHidden || sc->lastMatch == 0 || s_scatterFrame - sc->lastMatch > 600) {
      continue;
    }
    const float cx = s_cameraPos[0], cy = s_cameraPos[1], cz = s_cameraPos[2];
    const int64_t r = int64_t(std::ceil(sc->maxRange / kScatterCell));
    const int64_t ix = int64_t(std::floor(cx / kScatterCell)), iy = int64_t(std::floor(cy / kScatterCell));
    for (int64_t gx = ix - r; gx <= ix + r; ++gx) {
      for (int64_t gy = iy - r; gy <= iy + r; ++gy) {
        const auto cell = sc->cells.find(scatterCell(gx, gy));
        if (cell == sc->cells.end()) continue;
        for (const uint32_t i : cell->second) {
          const ScatterPlant& p = sc->plants[i];
          const float dx = p.m[0][3] - cx, dy = p.m[1][3] - cy, dz = p.m[2][3] - cz;
          if (dx * dx + dy * dy + dz * dz <= p.range2) {
            drawScatterInstance(sc->meshes[p.mesh], p.m);
          }
        }
      }
    }
  }
}
// ---- Vegetation editor (user 2026-10-03: "a brush that puts the plants on the ground at random, with species to
// include or exclude from the random choice, and every plant controllable on its own - size, position, delete - like
// Unreal's foliage brushes"). The panel is in the Remix runtime (fork, rtx_fork_foliage.cpp: M, tab "Vegetazione");
// it talks to this code through the shared block of foliage_shared.h. The mod casts the rays and owns the plants.
// Painted plants: <pbr>\vegetation\painted\plants.txt ("<folder> <obj> <name>" per mesh, the folder holding the
// species' albedo / normal / roughness .ac1t; tools/veg_painted_pack.py) and placed.txt (one plant per line, written
// by the mod). Each plant is anchored to the static mesh instance it stands on (content hash of the mesh + its
// position): drawn while that instance is drawn, so plants follow the level streaming and never show in another level.
// Ground: ray casts against the static geometry the mod submits (opaque ranges; alpha-tested foliage, blended and
// decal ranges and hidden far-LOD cells skipped), kept as positions and 32-triangle chunks per mesh.
struct CollisionChunk {
  float lo[3], hi[3];
  uint32_t first, count;  // triangles
};
struct CollisionSurface {
  uint32_t range = 0;
  uint32_t cell = static_geometry::Surface::kNoCell;
  std::vector<float> pos;
  std::vector<uint32_t> idx;
  std::vector<CollisionChunk> chunks;
};
struct CollisionMesh {
  uint64_t content = 0;
  float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
  std::vector<CollisionSurface> surfaces;
};
std::unordered_map<uint64_t, CollisionMesh> s_collision;
size_t s_collisionTriangles = 0;

void collisionAdd(const static_geometry::Mesh& mesh) {
  CollisionMesh cm;
  uint64_t h = 0xCBF29CE484222325ull;
  for (const auto& s : mesh.surfaces) {
    if (s.layer || s.blend || s.decal || s.indices.empty()) {
      continue;
    }
    CollisionSurface cs;
    cs.range = s.range;
    cs.cell = s.cell;
    cs.pos.reserve(s.vertices.size() * 3);
    for (const auto& v : s.vertices) {
      cs.pos.insert(cs.pos.end(), { v.position[0], v.position[1], v.position[2] });
    }
    cs.idx = s.indices;
    for (size_t i = 0; i < s.vertices.size(); i += 7) {
      for (int c = 0; c < 3; ++c) {
        uint32_t bits;
        std::memcpy(&bits, &s.vertices[i].position[c], 4);
        h = (h ^ bits) * 0x100000001B3ull;
      }
    }
    h = (h ^ s.indices.size()) * 0x100000001B3ull;
    const uint32_t tris = uint32_t(cs.idx.size() / 3);
    for (uint32_t first = 0; first < tris; first += 32) {
      CollisionChunk ch = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, first, std::min(32u, tris - first) };
      for (uint32_t t = first; t < first + ch.count; ++t) {
        for (int k = 0; k < 3; ++k) {
          const float* p = &cs.pos[size_t(cs.idx[t * 3 + k]) * 3];
          for (int c = 0; c < 3; ++c) {
            ch.lo[c] = std::min(ch.lo[c], p[c]);
            ch.hi[c] = std::max(ch.hi[c], p[c]);
          }
        }
      }
      for (int c = 0; c < 3; ++c) {
        cm.lo[c] = std::min(cm.lo[c], ch.lo[c]);
        cm.hi[c] = std::max(cm.hi[c], ch.hi[c]);
      }
      cs.chunks.push_back(ch);
    }
    s_collisionTriangles += tris;
    cm.surfaces.push_back(std::move(cs));
  }
  cm.content = h;
  if (!cm.surfaces.empty()) {
    s_collision[mesh.key] = std::move(cm);
  }
}

void collisionRemove(uint64_t key) {
  const auto it = s_collision.find(key);
  if (it != s_collision.end()) {
    for (const auto& s : it->second.surfaces) s_collisionTriangles -= s.idx.size() / 3;
    s_collision.erase(it);
  }
}

// Static instances drawn this frame (for rays and plant anchors).
struct FrameStatic {
  uint64_t meshKey;
  float m[3][4];
  float inv[3][4];
  std::vector<uint8_t> cells;
};
std::vector<FrameStatic> s_frameStatic;
std::unordered_set<uint64_t> s_drawnAnchors;

uint64_t anchorKey(uint64_t content, const float t[3]) {
  uint64_t h = content * 0x9E3779B97F4A7C15ull;
  for (int c = 0; c < 3; ++c) {
    h = (h ^ uint64_t(int64_t(std::lround(t[c] * 10.0f)))) * 0xC2B2AE3D27D4EB4Full;
  }
  return h ^ (h >> 29);
}

void invertAffine(const float m[3][4], float inv[3][4]) {
  const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
              i = m[2][2];
  const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-12f) det = 1e-12f;
  const float r = 1.0f / det;
  const float R[3][3] = { { A * r, -(b * i - c * h) * r, (b * f - c * e) * r },
                          { B * r, (a * i - c * g) * r, -(a * f - c * d) * r },
                          { C * r, -(a * h - b * g) * r, (a * e - b * d) * r } };
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) inv[y][x] = R[y][x];
    inv[y][3] = -(R[y][0] * m[0][3] + R[y][1] * m[1][3] + R[y][2] * m[2][3]);
  }
}

void collectFrameStatic(const std::vector<static_geometry::Instance>& instances) {
  s_frameStatic.clear();
  s_drawnAnchors.clear();
  for (const auto& in : instances) {
    const auto cm = s_collision.find(in.meshKey);
    if (cm == s_collision.end()) {
      continue;
    }
    FrameStatic fs;
    fs.meshKey = in.meshKey;
    const remixapi_Transform t = toRemixTransform(in.world);
    std::memcpy(fs.m, t.matrix, sizeof(fs.m));
    invertAffine(fs.m, fs.inv);
    fs.cells = in.cellVisible;
    const float tr[3] = { fs.m[0][3], fs.m[1][3], fs.m[2][3] };
    s_drawnAnchors.insert(anchorKey(cm->second.content, tr));
    s_frameStatic.push_back(std::move(fs));
  }
}

bool slab(const float o[3], const float invD[3], const float lo[3], const float hi[3], float tMax) {
  float t0 = 0.0f, t1 = tMax;
  for (int c = 0; c < 3; ++c) {
    float a = (lo[c] - o[c]) * invD[c], b = (hi[c] - o[c]) * invD[c];
    if (a > b) std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1) return false;
  }
  return true;
}

struct RayHit {
  float t = 0.0f;
  float p[3] = {}, n[3] = {};
  uint64_t anchor = 0;
};

// First opaque static surface along o + t d (d unit), t in (0, maxT].
bool raycast(const float o[3], const float d[3], float maxT, RayHit& out) {
  bool found = false;
  float best = maxT;
  for (const auto& fs : s_frameStatic) {
    const auto cmIt = s_collision.find(fs.meshKey);
    if (cmIt == s_collision.end()) continue;
    const CollisionMesh& cm = cmIt->second;
    float lo[3], ld[3], invD[3];
    for (int r = 0; r < 3; ++r) {
      lo[r] = fs.inv[r][0] * o[0] + fs.inv[r][1] * o[1] + fs.inv[r][2] * o[2] + fs.inv[r][3];
      ld[r] = fs.inv[r][0] * d[0] + fs.inv[r][1] * d[1] + fs.inv[r][2] * d[2];
      invD[r] = std::fabs(ld[r]) > 1e-12f ? 1.0f / ld[r] : 1e30f;
    }
    if (!slab(lo, invD, cm.lo, cm.hi, best)) continue;
    const void* staticMesh = staticMeshOf(fs.meshKey);
    for (const auto& s : cm.surfaces) {
      if (s.cell != static_geometry::Surface::kNoCell && !fs.cells.empty() &&
          (s.cell >= fs.cells.size() || !fs.cells[s.cell])) {
        continue;  // far-LOD cell the engine hides this frame
      }
      const auto state = s_surfaceState.find({ staticMesh, s.range, nullptr });
      if (state != s_surfaceState.end() && (state->second.alphaTest || state->second.glass || state->second.water)) {
        continue;  // foliage cards, glass, water
      }
      for (const auto& ch : s.chunks) {
        if (!slab(lo, invD, ch.lo, ch.hi, best)) continue;
        for (uint32_t t = ch.first; t < ch.first + ch.count; ++t) {
          const float* p0 = &s.pos[size_t(s.idx[t * 3]) * 3];
          const float* p1 = &s.pos[size_t(s.idx[t * 3 + 1]) * 3];
          const float* p2 = &s.pos[size_t(s.idx[t * 3 + 2]) * 3];
          const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
          const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
          const float pv[3] = { ld[1] * e2[2] - ld[2] * e2[1], ld[2] * e2[0] - ld[0] * e2[2], ld[0] * e2[1] - ld[1] * e2[0] };
          const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
          if (std::fabs(det) < 1e-12f) continue;
          const float id = 1.0f / det;
          const float tv[3] = { lo[0] - p0[0], lo[1] - p0[1], lo[2] - p0[2] };
          const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id;
          if (u < 0.0f || u > 1.0f) continue;
          const float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
          const float v = (ld[0] * qv[0] + ld[1] * qv[1] + ld[2] * qv[2]) * id;
          if (v < 0.0f || u + v > 1.0f) continue;
          const float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id;
          if (tt <= 1e-4f || tt >= best) continue;
          best = tt;
          found = true;
          out.t = tt;
          for (int c = 0; c < 3; ++c) out.p[c] = o[c] + d[c] * tt;
          // normal: local cross product through the inverse transpose
          const float nl[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
          float n[3];
          for (int c = 0; c < 3; ++c) n[c] = fs.inv[0][c] * nl[0] + fs.inv[1][c] * nl[1] + fs.inv[2][c] * nl[2];
          float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (len < 1e-12f) len = 1.0f;
          const float sgn = (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) > 0.0f ? -1.0f : 1.0f;
          for (int c = 0; c < 3; ++c) out.n[c] = n[c] / len * sgn;
          const float tr[3] = { fs.m[0][3], fs.m[1][3], fs.m[2][3] };
          out.anchor = anchorKey(cm.content, tr);
        }
      }
    }
  }
  return found;
}

// Ground straight below (x, y), searched from `top` down `depth` metres.
bool groundBelow(float x, float y, float top, float depth, RayHit& out) {
  const float o[3] = { x, y, top }, d[3] = { 0.0f, 0.0f, -1.0f };
  return raycast(o, d, depth, out);
}

struct PaintedMesh {
  remixapi_MeshHandle handle = nullptr;
  float radius = 0.3f, height = 0.5f;
  uint32_t species = 0;
};
struct PaintedPlant {
  uint32_t mesh = 0;
  float pos[3] = {}, up[3] = { 0, 0, 1 };
  float yaw = 0.0f, scale = 1.0f, lift = 0.0f;
  uint64_t anchor = 0;
  float m[3][4] = {};
};
struct PaintedSpecies {
  std::string folder, name;
  std::vector<uint32_t> meshes;
};
std::vector<PaintedMesh> s_paintedMeshes;
std::vector<PaintedSpecies> s_paintedSpecies;
std::vector<PaintedPlant> s_painted;
std::vector<std::vector<PaintedPlant>> s_paintedUndo;
bool s_paintedLoaded = false, s_paintedDirty = false;
ULONGLONG s_paintedEditTime = 0;
int32_t s_paintedSelected = -1;
uint32_t s_lastCmd = 0;
ULONGLONG s_lastCmdTime = 0;
std::mt19937 s_paintRng { 20261003u };
ac1foliage::Shared* s_foliage = nullptr;

std::wstring paintedDir() {
  return pbr::folder() + L"\\vegetation\\painted\\";
}

void composePlant(PaintedPlant& p) {
  const float* up = p.up;
  const float ref[3] = { std::cos(p.yaw), std::sin(p.yaw), 0.0f };
  float side[3] = { up[1] * ref[2] - up[2] * ref[1], up[2] * ref[0] - up[0] * ref[2], up[0] * ref[1] - up[1] * ref[0] };
  float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
  if (sl < 1e-6f) sl = 1.0f;
  for (float& c : side) c /= sl;
  const float fwd[3] = { side[1] * up[2] - side[2] * up[1], side[2] * up[0] - side[0] * up[2],
                         side[0] * up[1] - side[1] * up[0] };
  for (int r = 0; r < 3; ++r) {
    p.m[r][0] = fwd[r] * p.scale;
    p.m[r][1] = side[r] * p.scale;
    p.m[r][2] = up[r] * p.scale;
    p.m[r][3] = p.pos[r] + (r == 2 ? p.lift : 0.0f);
  }
}

void plantUp(const float n[3], float align, float up[3]) {
  for (int c = 0; c < 3; ++c) up[c] = (c == 2 ? 1.0f - align : 0.0f) + n[c] * align;
  float l = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
  if (l < 1e-6f) {
    up[0] = up[1] = 0.0f;
    up[2] = l = 1.0f;
  }
  for (int c = 0; c < 3; ++c) up[c] /= l;
}

void loadPainted() {
  s_paintedLoaded = true;
  const std::wstring dir = paintedDir();
  FILE* f = _wfopen((dir + L"plants.txt").c_str(), L"r");
  if (!f) {
    log::line("vegetation editor: no %lsplants.txt", dir.c_str());
    return;
  }
  std::map<std::string, remixapi_MaterialHandle> materials;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char folder[128] = {}, obj[128] = {}, name[128] = {};
    if (sscanf_s(line, "%127s %127s %127s", folder, unsigned(sizeof(folder)), obj, unsigned(sizeof(obj)), name,
                 unsigned(sizeof(name))) < 2) {
      continue;
    }
    const std::string key(folder), objName(obj);
    std::string display(name[0] ? name : folder);
    std::replace(display.begin(), display.end(), '_', ' ');
    auto sp = std::find_if(s_paintedSpecies.begin(), s_paintedSpecies.end(),
                           [&](const PaintedSpecies& s) { return s.folder == key; });
    if (sp == s_paintedSpecies.end()) {
      if (s_paintedSpecies.size() >= ac1foliage::kMaxSpecies) continue;
      s_paintedSpecies.push_back({ key, display, {} });
      sp = s_paintedSpecies.end() - 1;
    }
    const std::wstring folderDir = dir + std::wstring(key.begin(), key.end()) + L"\\";
    auto mat = materials.find(key);
    if (mat == materials.end()) {
      const uint64_t h = uint64_t(std::hash<std::string>()("painted/" + key)) * 0x9E3779B97F4A7C15ull;
      mat = materials.emplace(key, vegLoadMaterial(folderDir, 0xAC1F600000000000ull | (h & 0x00000FFFFFFFFFFFull))).first;
    }
    PaintedMesh pm;
    pm.species = uint32_t(sp - s_paintedSpecies.begin());
    std::vector<remixapi_HardcodedVertex> verts;
    std::vector<uint32_t> indices;
    const std::wstring objPath = folderDir + std::wstring(objName.begin(), objName.end());
    if (vegLoadObj(objPath, verts, indices)) {
      float r2 = 0.0f, h = 0.0f;
      for (const auto& v : verts) {
        r2 = std::max(r2, v.position[0] * v.position[0] + v.position[1] * v.position[1]);
        h = std::max(h, v.position[2]);
      }
      pm.radius = std::sqrt(r2);
      pm.height = h;
      remixapi_MeshInfoSurfaceTriangles surface = {};
      surface.vertices_values = verts.data();
      surface.vertices_count = verts.size();
      surface.indices_values = indices.data();
      surface.indices_count = indices.size();
      surface.material = mat->second;
      remixapi_MeshInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      const uint64_t mh = uint64_t(std::hash<std::string>()("painted/" + key + "/" + objName)) * 0xC2B2AE3D27D4EB4Full;
      info.hash = 0xAC1E600000000000ull | (mh & 0x00000FFFFFFFFFFFull);
      info.surfaces_values = &surface;
      info.surfaces_count = 1;
      if (s_api.CreateMesh(&info, &pm.handle) != REMIXAPI_ERROR_CODE_SUCCESS) pm.handle = nullptr;
    }
    sp->meshes.push_back(uint32_t(s_paintedMeshes.size()));
    s_paintedMeshes.push_back(pm);
  }
  fclose(f);
  if ((f = _wfopen((dir + L"placed.txt").c_str(), L"r"))) {
    while (fgets(line, sizeof(line), f)) {
      PaintedPlant p;
      unsigned long long anchor = 0;
      if (line[0] == 'P' && sscanf_s(line + 1, "%u %f %f %f %f %f %f %f %f %f %llx", &p.mesh, &p.pos[0], &p.pos[1],
                                     &p.pos[2], &p.up[0], &p.up[1], &p.up[2], &p.yaw, &p.scale, &p.lift, &anchor) == 11 &&
          p.mesh < s_paintedMeshes.size()) {
        p.anchor = anchor;
        composePlant(p);
        s_painted.push_back(p);
      }
    }
    fclose(f);
  }
  log::line("vegetation editor: %zu species, %zu meshes, %zu plants", s_paintedSpecies.size(), s_paintedMeshes.size(),
            s_painted.size());
}

void savePainted() {
  const std::wstring path = paintedDir() + L"placed.txt";
  if (FILE* f = _wfopen((path + L".tmp").c_str(), L"w")) {
    fprintf(f, "# AC1 RTX painted vegetation: P mesh x y z upx upy upz yaw scale lift anchor\n");
    for (const auto& p : s_painted) {
      fprintf(f, "P %u %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.4f %.4f %016llX\n", p.mesh, p.pos[0], p.pos[1], p.pos[2],
              p.up[0], p.up[1], p.up[2], p.yaw, p.scale, p.lift, static_cast<unsigned long long>(p.anchor));
    }
    fclose(f);
    MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    s_paintedDirty = false;
  }
}

void drawPainted() {
  if (s_scatterHidden) {
    return;  // F7: game vegetation only
  }
  for (const auto& p : s_painted) {
    const PaintedMesh& pm = s_paintedMeshes[p.mesh];
    if (!pm.handle || !s_drawnAnchors.count(p.anchor)) {
      continue;
    }
    const float range = std::clamp(30.0f + pm.height * p.scale * 60.0f, 30.0f, 160.0f);
    const float dx = p.m[0][3] - s_cameraPos[0], dy = p.m[1][3] - s_cameraPos[1], dz = p.m[2][3] - s_cameraPos[2];
    if (dx * dx + dy * dy + dz * dz > range * range) {
      continue;
    }
    drawScatterInstance(pm.handle, p.m);
  }
}

void foliageStatus(const char* fmt, ...) {
  if (!s_foliage) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_foliage->status, sizeof(s_foliage->status), fmt, args);
  va_end(args);
}

void pushUndo(uint32_t cmd) {
  const ULONGLONG now = GetTickCount64();
  // a brush stroke arrives as many commands: one undo step per stroke
  const bool sameStroke = (cmd == ac1foliage::kPaint || cmd == ac1foliage::kErase) && cmd == s_lastCmd &&
                          now - s_lastCmdTime < 400;
  s_lastCmd = cmd;
  s_lastCmdTime = now;
  if (sameStroke) return;
  s_paintedUndo.push_back(s_painted);
  if (s_paintedUndo.size() > 40) s_paintedUndo.erase(s_paintedUndo.begin());
}

void foliageCommand(uint32_t cmd) {
  using namespace ac1foliage;
  Shared& sh = *s_foliage;
  float o[3], d[3];
  std::memcpy(o, sh.rayOrigin, sizeof(o));
  std::memcpy(d, sh.rayDir, sizeof(d));
  RayHit hit;
  const bool hasHit = raycast(o, d, 3000.0f, hit);
  const auto edited = [] {
    s_paintedDirty = true;
    s_paintedEditTime = GetTickCount64();
  };
  switch (cmd) {
    case kPaint: {
      if (!hasHit) break;
      std::vector<uint32_t> species;
      for (uint32_t i = 0; i < s_paintedSpecies.size(); ++i) {
        if ((sh.speciesMask >> i & 1u) && !s_paintedSpecies[i].meshes.empty()) species.push_back(i);
      }
      if (species.empty()) {
        foliageStatus("Nessuna specie scelta");
        break;
      }
      pushUndo(cmd);
      std::uniform_real_distribution<float> u01(0.0f, 1.0f);
      const float r = std::max(0.1f, sh.radius);
      const float expected = sh.density * 3.14159265f * r * r * std::clamp(sh.strokeSeconds, 0.0f, 0.5f);
      int n = int(expected);
      if (u01(s_paintRng) < expected - float(n)) ++n;
      int placed = 0;
      for (int k = 0; k < n; ++k) {
        const float a = u01(s_paintRng) * 6.2831853f, rr = r * std::sqrt(u01(s_paintRng));
        const float x = hit.p[0] + rr * std::cos(a), y = hit.p[1] + rr * std::sin(a);
        RayHit g;
        if (!groundBelow(x, y, hit.p[2] + r + 2.0f, 2.0f * r + 6.0f, g)) continue;
        if (g.n[2] < 0.35f) continue;  // walls and cliffs
        const uint32_t s = species[size_t(u01(s_paintRng) * species.size()) % species.size()];
        const auto& meshes = s_paintedSpecies[s].meshes;
        PaintedPlant p;
        p.mesh = meshes[size_t(u01(s_paintRng) * meshes.size()) % meshes.size()];
        p.scale = sh.scaleMin + (sh.scaleMax - sh.scaleMin) * u01(s_paintRng);
        // spacing: not inside another plant's crown
        const float mine = s_paintedMeshes[p.mesh].radius * p.scale;
        bool free = true;
        for (const auto& q : s_painted) {
          const float qx = q.pos[0] - g.p[0], qy = q.pos[1] - g.p[1], qz = q.pos[2] - g.p[2];
          const float lim = 0.45f * (mine + s_paintedMeshes[q.mesh].radius * q.scale);
          if (qx * qx + qy * qy < lim * lim && std::fabs(qz) < 1.5f) {
            free = false;
            break;
          }
        }
        if (!free) continue;
        std::memcpy(p.pos, g.p, sizeof(p.pos));
        p.pos[2] -= 0.02f * p.scale;
        plantUp(g.n, std::clamp(sh.slopeAlign, 0.0f, 1.0f), p.up);
        p.yaw = u01(s_paintRng) * 6.2831853f;
        p.anchor = g.anchor;
        composePlant(p);
        s_painted.push_back(p);
        ++placed;
      }
      if (placed) edited();
      foliageStatus("Piante: %zu", s_painted.size());
      break;
    }
    case kErase: {
      if (!hasHit) break;
      pushUndo(cmd);
      const float r = std::max(0.1f, sh.radius);
      const size_t before = s_painted.size();
      s_painted.erase(std::remove_if(s_painted.begin(), s_painted.end(), [&](const PaintedPlant& p) {
                        const uint32_t s = s_paintedMeshes[p.mesh].species;
                        const float dx = p.pos[0] - hit.p[0], dy = p.pos[1] - hit.p[1];
                        return (sh.speciesMask >> s & 1u) && dx * dx + dy * dy < r * r &&
                               std::fabs(p.pos[2] - hit.p[2]) < r + 2.0f;
                      }),
                      s_painted.end());
      if (s_painted.size() != before) {
        edited();
        s_paintedSelected = -1;
      }
      foliageStatus("Piante: %zu", s_painted.size());
      break;
    }
    case kSelect: {
      // nearest plant whose crown sphere the ray crosses (in front of the ground hit)
      float best = hasHit ? hit.t + 0.5f : 3000.0f;
      int32_t found = -1;
      for (size_t i = 0; i < s_painted.size(); ++i) {
        const PaintedPlant& p = s_painted[i];
        const PaintedMesh& pm = s_paintedMeshes[p.mesh];
        float c[3];
        for (int k = 0; k < 3; ++k) c[k] = p.m[k][3] + p.up[k] * pm.height * p.scale * 0.5f;
        const float rad = std::max(0.15f, std::max(pm.radius, pm.height * 0.5f) * p.scale * 0.8f);
        const float oc[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] };
        const float tc = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        if (tc < 0.0f) continue;
        const float d2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tc * tc;
        if (d2 > rad * rad) continue;
        const float t = tc - std::sqrt(std::max(0.0f, rad * rad - d2));
        if (t < best) {
          best = t;
          found = int32_t(i);
        }
      }
      s_paintedSelected = found;
      foliageStatus(found >= 0 ? "Pianta selezionata" : "Nessuna pianta qui");
      break;
    }
    case kMoveSelected: {
      if (!hasHit || s_paintedSelected < 0 || size_t(s_paintedSelected) >= s_painted.size()) break;
      pushUndo(cmd);
      PaintedPlant& p = s_painted[s_paintedSelected];
      std::memcpy(p.pos, hit.p, sizeof(p.pos));
      p.pos[2] -= 0.02f * p.scale;
      plantUp(hit.n, std::clamp(sh.slopeAlign, 0.0f, 1.0f), p.up);
      p.anchor = hit.anchor;
      composePlant(p);
      edited();
      break;
    }
    case kUpdateSelected: {
      if (s_paintedSelected < 0 || size_t(s_paintedSelected) >= s_painted.size()) break;
      pushUndo(cmd);
      PaintedPlant& p = s_painted[s_paintedSelected];
      p.scale = std::clamp(sh.selScale, 0.05f, 20.0f);
      p.yaw = sh.selYaw * 3.14159265f / 180.0f;
      p.lift = std::clamp(sh.selLift, -2.0f, 2.0f);
      composePlant(p);
      edited();
      break;
    }
    case kDeleteSelected:
      if (s_paintedSelected >= 0 && size_t(s_paintedSelected) < s_painted.size()) {
        pushUndo(cmd);
        s_painted.erase(s_painted.begin() + s_paintedSelected);
        s_paintedSelected = -1;
        edited();
      }
      break;
    case kDeselect:
      s_paintedSelected = -1;
      break;
    case kUndo:
      if (!s_paintedUndo.empty()) {
        s_painted = std::move(s_paintedUndo.back());
        s_paintedUndo.pop_back();
        s_paintedSelected = -1;
        s_lastCmd = 0;
        edited();
        foliageStatus("Annullato. Piante: %zu", s_painted.size());
      }
      break;
    case kClearAll:
      pushUndo(cmd);
      s_painted.clear();
      s_paintedSelected = -1;
      edited();
      foliageStatus("Tutte le piante tolte (Annulla per riaverle)");
      break;
    case kSave:
      savePainted();
      foliageStatus("Salvato: %zu piante", s_painted.size());
      break;
    default:
      break;
  }
}

// Every frame on the render thread, after the static instances of the frame are known.
void foliageTick() {
  using namespace ac1foliage;
  if (!pbr::enabled()) {
    return;
  }
  if (!s_paintedLoaded) {
    loadPainted();
  }
  if (!s_foliage) {
    static bool s_tried = false;
    if (s_tried) return;
    s_tried = true;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kMappingName);
    if (!map) {
      log::line("vegetation editor: shared memory not created (%lu)", GetLastError());
      return;
    }
    s_foliage = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!s_foliage) return;
    std::memset(s_foliage, 0, sizeof(Shared));
    s_foliage->version = kVersion;
    s_foliage->selected = -1;
    MemoryBarrier();
    s_foliage->magic = kMagic;
    log::line("vegetation editor: shared memory ready, %zu collision triangles", s_collisionTriangles);
  }
  Shared& sh = *s_foliage;
  sh.speciesCount = uint32_t(s_paintedSpecies.size());
  for (size_t i = 0; i < s_paintedSpecies.size(); ++i) {
    strncpy_s(sh.species[i], s_paintedSpecies[i].name.c_str(), _TRUNCATE);
  }
  const uint32_t seq = sh.cmdSeq;
  if (seq != sh.ackSeq) {
    MemoryBarrier();
    foliageCommand(sh.cmd);
    sh.ackSeq = seq;
  }
  if (sh.editorOpen) {
    float o[3], d[3];
    std::memcpy(o, sh.rayOrigin, sizeof(o));
    std::memcpy(d, sh.rayDir, sizeof(d));
    RayHit hit;
    sh.hitValid = raycast(o, d, 3000.0f, hit) ? 1u : 0u;
    if (sh.hitValid) {
      std::memcpy(sh.hit, hit.p, sizeof(sh.hit));
      std::memcpy(sh.hitNormal, hit.n, sizeof(sh.hitNormal));
    }
  }
  sh.plantCount = uint32_t(s_painted.size());
  sh.undoDepth = uint32_t(s_paintedUndo.size());
  sh.selected = s_paintedSelected;
  if (s_paintedSelected >= 0 && size_t(s_paintedSelected) < s_painted.size()) {
    const PaintedPlant& p = s_painted[s_paintedSelected];
    const PaintedMesh& pm = s_paintedMeshes[p.mesh];
    sh.selSpecies = pm.species;
    for (int k = 0; k < 3; ++k) sh.selPos[k] = p.m[k][3];
    sh.selScaleOut = p.scale;
    float yaw = p.yaw * 180.0f / 3.14159265f;
    yaw = std::fmod(yaw, 360.0f);
    if (yaw < 0.0f) yaw += 360.0f;
    sh.selYawOut = yaw;
    sh.selLiftOut = p.lift;
    sh.selRadius = pm.radius * p.scale;
    sh.selHeight = pm.height * p.scale;
  }
  if (s_paintedDirty && GetTickCount64() - s_paintedEditTime > 1500) {
    savePainted();  // autosave after an edit
  }
}

using ClutterKey = std::tuple<uint64_t, const void*, bool>;  // (geometry hash, matInst, double-sided)
std::map<ClutterKey, remixapi_MeshHandle> s_clutterMeshes;
uint64_t s_clutterInstancesLogged = 0;

void submitDynamic() {
  dynamic_mesh::Frame frame = dynamic_mesh::takeFrame();
  ++s_scatterFrame;
  static bool s_f7Down = false;
  const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
  if (f7 && !s_f7Down) {
    s_scatterHidden = !s_scatterHidden;
    log::line("vegetation scatter %s (F7)", s_scatterHidden ? "OFF (original grass)" : "ON");
  }
  s_f7Down = f7;
  for (remixapi_MeshHandle handle : s_dynamicMeshes) {
    s_api.DestroyMesh(handle);
  }
  s_dynamicMeshes.clear();
  uint64_t clutterInstances = 0;
  for (const auto& draw : frame.draws) {
    const uint64_t meshKey = 0xD100000000000000ull | reinterpret_cast<uintptr_t>(draw.resource);
    s_meshKeyOf[draw.resource] = meshKey;
    const bool instanced = !draw.instances.empty();
    const ClutterKey clutterKey { draw.geometryHash, draw.matInst, draw.doubleSided };
    if (instanced) {
      if (Scatter* sc = scatterFor(draw.geometryHash)) {
        if (pbr::exportVegetation()) {
          for (const auto& t : draw.instances) {
            remixapi_Transform w;
            std::memcpy(&w, t.m, sizeof(w));
            vegPlace(draw.geometryHash, w);
          }
        }
        scatterTufts(*sc, draw.instances);
        continue;
      }
      auto it = s_clutterMeshes.find(clutterKey);
      if (it != s_clutterMeshes.end()) {
        if (pbr::exportVegetation()) {
          for (const auto& t : draw.instances) {
            remixapi_Transform w;
            std::memcpy(&w, t.m, sizeof(w));
            vegPlace(draw.geometryHash, w);
          }
        }
        if (it->second) {
          for (const auto& t : draw.instances) {
            remixapi_InstanceInfo inst = {};
            inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            inst.mesh = it->second;
            static_assert(sizeof(inst.transform) == sizeof(t.m));
            std::memcpy(&inst.transform, t.m, sizeof(inst.transform));
            inst.doubleSided = draw.doubleSided ? 1 : 0;
            drawInstance(&inst);
          }
          clutterInstances += draw.instances.size();
        }
        continue;
      }
    }
    std::vector<remixapi_HardcodedVertex> verts(draw.vertices.size());
    for (size_t v = 0; v < verts.size(); ++v) {
      verts[v] = {};
      std::memcpy(verts[v].position, draw.vertices[v].position, sizeof(verts[v].position));
      std::memcpy(verts[v].normal, draw.vertices[v].normal, sizeof(verts[v].normal));
      std::memcpy(verts[v].texcoord, draw.vertices[v].texcoord, sizeof(verts[v].texcoord));
      verts[v].color = 0xFFFFFFFF;
    }
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = verts.data();
    surface.vertices_count = verts.size();
    surface.indices_values = draw.indices.data();
    surface.indices_count = draw.indices.size();
    ensureMetresPerUv(draw.resource, 0, draw.vertices, draw.indices);
    // dynamic meshes = simulated cloth (Altair's robe, awnings) and instanced clutter: engine materials like the
    // skinned ones (the robe's PS tints the texture, a PBR albedo turned it grey; vegetation is to be rebuilt)
    surface.material = surfaceMaterial(meshKey, draw.resource, 0, draw.matInst, 0, 0, true);
    if (!surface.material) {
      surface.material = s_staticMaterial;
    }
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = instanced ? (draw.geometryHash * 0x9E3779B97F4A7C15ull) ^
                                (uint64_t(reinterpret_cast<uintptr_t>(draw.matInst)) + 1) * 0x94D049BB133111EBull ^
                                (draw.doubleSided ? 0xD3D3D3D3D3D3D3D3ull : 0)
                          : 0xD200000000000000ull | (++s_dynamicSerial & 0x00FFFFFFFFFFFFFFull);
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle handle = instanced ? vegReplacementMesh(draw.geometryHash) : nullptr;
    if (!handle && (s_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle)) {
      handle = nullptr;
    }
    if (instanced) {
      s_clutterMeshes[clutterKey] = handle;  // kept (also when creation failed, so it is not retried every frame)
      const auto state = s_surfaceState.find({ draw.resource, 0u, draw.matInst });
      vegExport(draw.geometryHash, "clutter", draw.vertices, draw.indices,
                state != s_surfaceState.end() ? &state->second : nullptr);
      log::line("clutter mesh %016llx: %zu vertices, %zu triangles, matInst %p, %zu instances -> %p",
                static_cast<unsigned long long>(draw.geometryHash), draw.vertices.size(), draw.indices.size() / 3,
                draw.matInst, draw.instances.size(), handle);
    }
    if (!handle) {
      continue;
    }
    if (instanced) {
      for (const auto& t : draw.instances) {
        remixapi_InstanceInfo inst = {};
        inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        inst.mesh = handle;
        std::memcpy(&inst.transform, t.m, sizeof(inst.transform));
        inst.doubleSided = draw.doubleSided ? 1 : 0;
        drawInstance(&inst);
      }
      clutterInstances += draw.instances.size();
      continue;
    }
    s_dynamicMeshes.push_back(handle);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = handle;
    inst.transform = toRemixTransform(draw.world);
    inst.doubleSided = draw.doubleSided ? 1 : 0;
    drawInstance(&inst);
  }
  drawScatterPlans();
  drawPainted();
  if (clutterInstances > s_clutterInstancesLogged) {
    s_clutterInstancesLogged = clutterInstances;
    log::line("clutter: %llu instances this frame (%zu meshes kept)", static_cast<unsigned long long>(clutterInstances),
              s_clutterMeshes.size());
  }
}
// ---- Lights (phase 9) ----

// Light channels (game.h): the engine lights an object only when its channel bits meet the light's. Per channel bit
// the mod counts unique placements submitted as world geometry (static + masked) and as skinned characters, every
// frame. Measured live in Masyaf: terrain 0x01 only; static 0x01 (171 meshes) and one moving static attached to a
// character on 0x02; skinned 0x02 for 100 meshes / 211 instances, 0x01 for 3 world skinned meshes. A bit is a world
// channel when world placements use it at least as often as skinned ones; a light that meets no world channel only
// lights characters in the engine (a "fake" light: in Masyaf the second SunLight, channel 0x02) and is not exported.
uint32_t worldChannels() {
  uint32_t mask = 0;
  for (int bit = 0; bit < 8; ++bit) {
    if (s_worldPlacements[bit] > 0 && s_worldPlacements[bit] >= s_skinnedPlacements[bit]) {
      mask |= 1u << bit;
    }
  }
  return mask;
}

struct RemixLight {
  remixapi_LightHandle handle = nullptr;
  lights::Light source;
};
std::map<uint64_t, RemixLight> s_lights;  // (L, R) -> Remix light
std::set<uint64_t> s_excludedLogged;
float s_sunElevation = 1000.0f, s_sunRotation = 1000.0f;

uint64_t lightKey(const lights::Light& l) {
  return (uint64_t(reinterpret_cast<uintptr_t>(l.object)) << 32) | reinterpret_cast<uintptr_t>(l.resource);
}

Vec3 normalized(const float v[3]) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return len > 0.0f ? Vec3 { v[0] / len, v[1] / len, v[2] / len } : Vec3 { 0.0f, 0.0f, -1.0f };
}

// Omni/spot: engine term albedo * rgb * att(d) * N.L with att = sat((F^2 - d^2) / (F^2 - N^2)) (VS g_OmniLights);
// a Remix sphere light of radiance L and radius r gives albedo * L * r^2 / d^2 * N.L (Lambert albedo/pi times
// irradiance L*pi*r^2/d^2). The engine falloff has no Remix equivalent, so the radiance is chosen to deliver the same
// total flux inside the engine's range: L * r^2 * F = rgb * integral_0^F d^2 att(d) dd.
float fluxMatchedRadianceFactor(float nearDistance, float farDistance, float radius) {
  const float f = std::max(farDistance, 1e-3f);
  const float n = std::clamp(nearDistance, 0.0f, f);
  float integral = n * n * n / 3.0f;
  const float span = f * f - n * n;
  if (span > 1e-6f) {
    auto primitive = [f](float d) { return f * f * d * d * d / 3.0f - d * d * d * d * d / 5.0f; };
    integral += (primitive(f) - primitive(n)) / span;
  }
  return integral / (f * radius * radius);
}

void fillLightInfo(const lights::Light& l, remixapi_LightInfo& info, remixapi_LightInfoSphereEXT& sphere,
                   remixapi_LightInfoDistantEXT& distant) {
  info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = lightKey(l);
  info.isDynamic = true;  // parameters may change; non-dynamic lights stop taking updates (rtx_fork_light.cpp:131)
  const float radius = std::max(s_settings.lightSphereRadius, 1e-3f);
  float scale = s_settings.lightRadianceScale;
  if (l.kind == lights::Kind::Directional) {
    distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    const Vec3 d = normalized(l.axisY);  // travel direction = +Y (the shaders get -Y as direction to the light)
    distant.direction = { d.x, d.y, d.z };
    distant.angularDiameterDegrees = 0.5f;  // not engine data: the engine's directional lights have no size
    distant.volumetricRadianceScale = 1.0f;
    info.pNext = &distant;
  } else {
    sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = { l.position[0], l.position[1], l.position[2] };
    sphere.radius = radius;
    sphere.volumetricRadianceScale = 1.0f;
    if (l.kind == lights::Kind::Spot) {
      // Engine cone (FUN_00AD4AC0): linear ramp from cos(outer) (0) to cos(inner) (1) around +Y.
      // Remix: smoothstep(cos(cone), cos(cone) + softness, cos(theta)) around `direction` (light_shaping.slangh).
      const Vec3 d = normalized(l.axisY);
      sphere.shaping_hasvalue = true;
      sphere.shaping_value.direction = { d.x, d.y, d.z };
      sphere.shaping_value.coneAngleDegrees = l.outerAngle * 57.2957795f;
      sphere.shaping_value.coneSoftness = std::max(0.0f, std::cos(l.innerAngle) - std::cos(l.outerAngle));
      sphere.shaping_value.focusExponent = 0.0f;
    }
    scale *= fluxMatchedRadianceFactor(l.nearDistance, l.farDistance, radius);
    info.pNext = &sphere;
  }
  info.radiance = { l.color[0] * scale, l.color[1] * scale, l.color[2] * scale };
}

bool sameLight(const lights::Light& a, const lights::Light& b) {
  return a.kind == b.kind && std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
         std::memcmp(a.axisY, b.axisY, sizeof(a.axisY)) == 0 && std::memcmp(a.color, b.color, sizeof(a.color)) == 0 &&
         a.nearDistance == b.nearDistance && a.farDistance == b.farDistance && a.innerAngle == b.innerAngle &&
         a.outerAngle == b.outerAngle;
}

// rtx.skyMode = Numos: the atmosphere owns the sun light; the engine sun only sets its direction.
// Remix: toSun(Y-up) = (cos e sin a, sin e, cos e cos a), world = (x, z, y) with rtx.zUp (rtx_atmosphere.cpp:588-592,
// 2848-2851), so for the engine's direction to the sun t = -Y: e = asin(t.z), a = atan2(t.x, t.y).
void driveSun(const lights::Light& sun) {
  const Vec3 t = normalized(sun.axisY);
  const float elevation = std::asin(std::clamp(-t.z, -1.0f, 1.0f)) * 57.2957795f;
  float rotation = std::atan2(-t.x, -t.y) * 57.2957795f;
  if (rotation < 0.0f) {
    rotation += 360.0f;
  }
  if (std::fabs(elevation - s_sunElevation) < 0.01f && std::fabs(rotation - s_sunRotation) < 0.01f) {
    return;
  }
  char value[32];
  std::snprintf(value, sizeof(value), "%.4f", elevation);
  const remixapi_ErrorCode e = s_api.SetConfigVariable("rtx.atmosphere.sunElevation", value);
  std::snprintf(value, sizeof(value), "%.4f", rotation);
  const remixapi_ErrorCode r = s_api.SetConfigVariable("rtx.atmosphere.sunRotation", value);
  log::line("sun %p: dir-to-sun (%.4f %.4f %.4f) -> sunElevation %.2f (%d) sunRotation %.2f (%d)", sun.object, -t.x,
            -t.y, -t.z, elevation, e, rotation, r);
  s_sunElevation = elevation;
  s_sunRotation = rotation;
}

const char* kindName(lights::Kind k) {
  switch (k) {
    case lights::Kind::Omni: return "omni";
    case lights::Kind::Spot: return "spot";
    case lights::Kind::Directional: return "directional";
    default: return "sun";
  }
}

void submitLights() {
  lights::Frame frame = lights::takeFrame();
  const uint32_t world = worldChannels();
  if (!frame.valid || world == 0) {
    return;  // no engine light update this frame, or no world geometry seen yet to tell real from fake lights
  }
  std::set<uint64_t> present;
  bool sunDriven = false;
  for (const auto& l : frame.lights) {
    const uint64_t key = lightKey(l);
    if ((l.channels & world) == 0) {
      if (s_excludedLogged.insert(key).second) {
        log::line("light %p (%s, channels %02X) excluded: lights no world geometry (world channels %02X)", l.object,
                  kindName(l.kind), l.channels, world);
      }
      continue;
    }
    if (l.kind == lights::Kind::Sun) {
      if (!sunDriven) {
        driveSun(l);
        sunDriven = true;
      }
      continue;
    }
    present.insert(key);
    remixapi_LightInfo info;
    remixapi_LightInfoSphereEXT sphere;
    remixapi_LightInfoDistantEXT distant;
    auto it = s_lights.find(key);
    if (it == s_lights.end()) {
      fillLightInfo(l, info, sphere, distant);
      remixapi_LightHandle handle = nullptr;
      const remixapi_ErrorCode err = s_api.CreateLight(&info, &handle);
      log::line("light %p (%s, channels %02X) pos (%.2f %.2f %.2f) +Y (%.3f %.3f %.3f) rgb*I (%.3f %.3f %.3f) "
                "near %.2f far %.2f cone %.3f/%.3f -> radiance (%.3f %.3f %.3f) CreateLight %d",
                l.object, kindName(l.kind), l.channels, l.position[0], l.position[1], l.position[2], l.axisY[0],
                l.axisY[1], l.axisY[2], l.color[0], l.color[1], l.color[2], l.nearDistance, l.farDistance,
                l.innerAngle, l.outerAngle, info.radiance.x, info.radiance.y, info.radiance.z, err);
      if (err == REMIXAPI_ERROR_CODE_SUCCESS) {
        s_lights.emplace(key, RemixLight { handle, l });
      }
    } else if (!sameLight(it->second.source, l)) {
      fillLightInfo(l, info, sphere, distant);
      s_api.UpdateLightDefinition(it->second.handle, &info);
      it->second.source = l;
    }
  }
  // Engine-faithful: a light the engine no longer lists is not applied by it either.
  for (auto it = s_lights.begin(); it != s_lights.end();) {
    if (!present.count(it->first)) {
      s_api.DestroyLight(it->second.handle);
      it = s_lights.erase(it);
    } else {
      ++it;
    }
  }
}

void submitStaticGeometry() {
  static_geometry::Frame frame = static_geometry::takeFrame();
  for (int bit = 0; bit < 8; ++bit) {
    s_worldPlacements[bit] += frame.channelPlacements[bit];
  }
  applyMaterialUpdates();
  collectUnusedTextures();
  pollPbrEdits();
  for (remixapi_MeshHandle handle : s_waterMeshes) {
    s_api.DestroyMesh(handle);
  }
  s_waterMeshes.clear();
  for (const auto& mesh : frame.newMeshes) {
    createStaticMesh(mesh);
  }
  collectFrameStatic(frame.instances);
  foliageTick();
  submitSkinned(frame.destroyedMeshes);
  submitDynamic();
  for (uint64_t key : frame.destroyedMeshes) {
    forgetMeshMaterials(key);
    collisionRemove(key);
    auto it = s_staticMeshes.find(key);
    if (it != s_staticMeshes.end()) {
      for (const auto& part : it->second) {
        s_api.DestroyMesh(part.handle);
      }
      s_staticMeshes.erase(it);
    }
  }
  for (const auto& instance : frame.instances) {
    auto it = s_staticMeshes.find(instance.meshKey);
    if (it == s_staticMeshes.end()) {
      continue;
    }
    const remixapi_Transform transform = toRemixTransform(instance.world);
    for (const auto& part : it->second) {
      // Masked meshes: draw only the cells the engine draws this frame (its far-LOD cell mask).
      if (part.cell != static_geometry::Surface::kNoCell && !instance.cellVisible.empty() &&
          (part.cell >= instance.cellVisible.size() || !instance.cellVisible[part.cell])) {
        continue;
      }
      drawStatic(instance.meshKey, part, transform);
    }
  }
}

// Measurement aid: F9 cycles Remix's debug view (debug_view_indices.h): shading normal (16, normal maps applied),
// geometry normal (15, interpolated vertex normals), off. Identical 16/15 images mean no normal map reaches Remix.
void pollDebugViewKey() {
  static bool s_wasDown = false;
  static int s_step = 0;
  const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  if (down && !s_wasDown) {
    static const char* kViews[] = { "16", "15", "0" };
    const char* value = kViews[s_step];
    s_step = (s_step + 1) % 3;
    log::line("debug view -> %s (%d)", value, s_api.SetConfigVariable("rtx.debugView.debugViewIdx", value));
  }
  s_wasDown = down;
}

// Per-frame submission (camera, world, lights). Runs at the engine's 2D overlay dispatch (beginOverlay) when the hud
// hook is installed - so the overlay can be rasterized on top - otherwise at Present.
double s_submitMs = 0.0;  // accumulated for the perf log
std::atomic<bool> s_frameSubmitted { false };  // this game frame was submitted (reset by endOverlay)
std::atomic<uint32_t> s_overlayEnds { 0 };      // overlay passes completed (onPresent fallback)
bool s_loggedInjectionMarker = false;

void submitFrame() {
  LARGE_INTEGER start, end, freq;
  QueryPerformanceCounter(&start);
  const camera::Snapshot frame = camera::takeFrame();
  if (!frame.valid) {
    return;
  }

  remixapi_CameraInfo cameraInfo = {};
  cameraInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  cameraInfo.type = REMIXAPI_CAMERA_TYPE_WORLD;
  // Engine matrices go in unchanged: Remix builds Matrix4 from float[4][4] the same way its D3D9 path
  // converts a D3DMATRIX (verified in dxvk-remix ConvertMatrix / toRtCamera).
  static_assert(sizeof(cameraInfo.view) == sizeof(frame.view.m));
  std::memcpy(cameraInfo.view, frame.view.m, sizeof(cameraInfo.view));
  std::memcpy(cameraInfo.projection, frame.projection.m, sizeof(cameraInfo.projection));
  s_api.SetupCamera(&cameraInfo);
  for (int i = 0; i < 3; ++i) {
    s_cameraPos[i] = -(frame.view.m[12 + 0] * frame.view.m[i * 4 + 0] + frame.view.m[12 + 1] * frame.view.m[i * 4 + 1] +
                       frame.view.m[12 + 2] * frame.view.m[i * 4 + 2]);
  }

  if (s_settings.staticMeshes) {
    submitStaticGeometry();
  }
  if (s_settings.lights) {
    submitLights();
  }
  QueryPerformanceCounter(&end);
  QueryPerformanceFrequency(&freq);
  s_submitMs += 1000.0 * double(end.QuadPart - start.QuadPart) / double(freq.QuadPart);
}

void REMIXAPI_CALL onPresent() {
  pollDebugViewKey();
  static bool s_f8Down = false;
  const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
  if (f8 && !s_f8Down) {
    s_hideGlass = !s_hideGlass;
    log::line("glass %s (F8)", s_hideGlass ? "HIDDEN" : "shown");
  }
  s_f8Down = f8;
  // The frame is submitted on the game's render thread at its post-process chain or overlay (hud hooks) and the
  // per-frame state is reset when the overlay returns: this callback is not ordered with the game thread (a reset
  // here landed between the post-process chain and the overlay). Present submits only when no
  // overlay pass ran for a few presents (loading screens, videos, or the hud hook disabled).
  static uint32_t s_lastOverlayEnds = 0, s_presentsWithoutOverlay = 0;
  const uint32_t overlayEnds = s_overlayEnds.load();
  if (overlayEnds != s_lastOverlayEnds) {
    s_lastOverlayEnds = overlayEnds;
    s_presentsWithoutOverlay = 0;
  } else if (++s_presentsWithoutOverlay > 2) {
    submitFrame();
  }

  // Cost log every 300 frames: frame time (present to present), mod CPU time submitting, instances sent.
  static LARGE_INTEGER s_freq = {}, s_lastPresent = {};
  static double s_frameMs = 0.0;
  static uint64_t s_instances = 0;
  static uint32_t s_frames = 0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!s_freq.QuadPart) {
    QueryPerformanceFrequency(&s_freq);
  }
  if (s_lastPresent.QuadPart) {
    s_frameMs += 1000.0 * double(now.QuadPart - s_lastPresent.QuadPart) / double(s_freq.QuadPart);
  }
  double& s_modMs = s_submitMs;
  s_lastPresent = now;
  s_instances += s_frameInstances;
  s_frameInstances = 0;
  if (++s_frames == 300) {
    static uint64_t s_lastMaterials = 0;
    log::line("perf: frame %.2f ms (%.1f fps), mod submit %.2f ms, instances %.0f per frame, materials created %.1f "
              "per frame", s_frameMs / 300.0, 300000.0 / s_frameMs, s_modMs / 300.0, double(s_instances) / 300.0,
              double(s_materialsCreated - s_lastMaterials) / 300.0);
    s_lastMaterials = s_materialsCreated;
    s_frameMs = s_modMs = 0.0;
    s_instances = 0;
    s_frames = 0;
  }
}

} // namespace

// Game post-process (colour-grading LUT) over the ray-traced image, toggled with I (hud.cpp kPostProcessChain).
bool s_gamePostProcess = true;

void markInjection(bool skipDraw) {
  const remixapi_ErrorCode marker =
    s_api.InjectRTXAtNextDraw ? s_api.InjectRTXAtNextDraw(skipDraw) : REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  if (!s_loggedInjectionMarker) {
    s_loggedInjectionMarker = true;
    log::line("InjectRTXAtNextDraw(skipDraw=%d) -> %d", skipDraw ? 1 : 0, marker);
  }
}

void beginPostProcess() {
  static bool s_wasDown = false;
  const bool down = (GetAsyncKeyState('I') & 0x8000) != 0;
  if (down && !s_wasDown) {
    s_gamePostProcess = !s_gamePostProcess;
    log::line("game post-process (LUT) %s (I)", s_gamePostProcess ? "ON" : "OFF");
  }
  s_wasDown = down;
  if (!s_initialized || !s_gamePostProcess || s_frameSubmitted || !camera::mainViewSeen()) {
    return;
  }
  // Remix writes the frame into the LUT's input instead of the engine's scene copy; the game's colour grading, final
  // copy and HUD then draw on top of it.
  submitFrame();
  s_frameSubmitted = true;
  markInjection(true);
}

void onViewSetup(bool secondaryView) {
  if (!s_initialized || !s_api.RasterizeOffscreenDraws) {
    return;
  }
  s_api.RasterizeOffscreenDraws(secondaryView);
}

void beginRenderTargetDraw() {
  // Only when the target's producer view ran this frame: in the Animus lab (no menu) the band surface is still drawn
  // with the menu's 2048x512 target (log 20:26) but no 2048 view runs; injecting there rasterized every later
  // transparent draw (light shafts) over the ray-traced image without depth.
  if (!s_initialized || s_frameSubmitted || !camera::mainViewSeen() || !camera::secondaryViewSeen()) {
    return;
  }
  // Remix writes into the scene target; the band draw, the game's post-process (LUT applied to everything) and
  // the overlay rasterize on top. The post-process chain is not skipped here.
  submitFrame();
  s_frameSubmitted = true;
  markInjection(false);
}

void endOverlay() {
  if (!s_frameSubmitted) {
    return;  // overlay of a view before the scene view: the frame continues
  }
  s_frameSubmitted = false;
  ++s_overlayEnds;
}

void beginOverlay() {
  if (!s_initialized || s_frameSubmitted || !camera::mainViewSeen()) {
    return;  // already injected at the post-process chain, or an overlay pass before the scene view
  }
  submitFrame();
  s_frameSubmitted = true;
  markInjection(false);
}

void configure(const Settings& settings) {
  s_settings = settings;
}

void ensureInitialized() {
  if (s_initialized || s_failed) {
    return;
  }
  const remixapi_ErrorCode init = remixapi::bridge_initRemixApi(&s_api);
  log::line("bridge_initRemixApi -> %d", init);
  if (init != REMIXAPI_ERROR_CODE_SUCCESS) {
    s_failed = true;
    return;
  }
  const remixapi_ErrorCode callbacks = remixapi::bridge_setRemixApiCallbacks(nullptr, nullptr, &onPresent);
  log::line("bridge_setRemixApiCallbacks -> %d", callbacks);
  if (callbacks != REMIXAPI_ERROR_CODE_SUCCESS) {
    s_failed = true;
    return;
  }
  if (s_settings.staticMeshes) {
    createStaticMaterial();
  }
  s_initialized = true;
}

} // namespace ac1rtx::remix
