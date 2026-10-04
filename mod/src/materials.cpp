#include "materials.h"

#include "dynamic_mesh.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "skinned.h"
#include "specular_table.h"
#include "static_geometry.h"

#include <d3d9.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ac1rtx::materials {

namespace {

using EndDrawFn = void(__thiscall*)(void* matInst, uint32_t record, void* device, void* item);
EndDrawFn s_originalEndDraw = nullptr;

std::mutex s_mutex;
using SurfaceKey = std::tuple<const void*, uint32_t, const void*>;  // (DX9StaticMesh*, range, skinned matInst)
std::map<SurfaceKey, SurfaceMaterial> s_current;
std::vector<Update> s_updates;

struct Sampler {
  std::string name;
  uint32_t reg = 0;
};
struct ShaderSamplers {
  int albedo = -1;
  int normal = -1;
};
std::unordered_map<IDirect3DPixelShader9*, ShaderSamplers> s_shaderSamplers;  // registers, -1 if none
std::unordered_map<IDirect3DPixelShader9*, const specular_table::Entry*> s_shaderSpecular;  // nullptr: not in census
std::unordered_map<IDirect3DPixelShader9*, uint64_t> s_shaderHash;                          // FNV-1a 64 of bytecode
std::wstring s_shaderDumpDir;
// MaterialInstances whose colour-pass draw binds a render-target colour texture (Animus menu band, 19:33 log).
std::mutex s_rtMutex;
std::unordered_set<const void*> s_rtMatInsts;
std::set<std::pair<const void*, uint32_t>> s_rtSurfaces;  // their (DX9StaticMesh*, range)
std::map<std::pair<const void*, uint32_t>, std::array<float, 6>> s_waterPanner;  // (mesh, range) -> Panner01 rows

// Samplers declared in the shader's constant table (CTAB comment block, D3DXSHADER_CONSTANTTABLE layout).
std::vector<Sampler> readSamplers(IDirect3DPixelShader9* shader) {
  std::vector<Sampler> samplers;
  UINT size = 0;
  if (FAILED(shader->GetFunction(nullptr, &size)) || size < 8) {
    return samplers;
  }
  std::vector<uint32_t> code(size / 4);
  if (FAILED(shader->GetFunction(code.data(), &size))) {
    return samplers;
  }
  for (size_t i = 1; i < code.size(); ++i) {
    const uint32_t token = code[i];
    if ((token & 0xFFFF) != 0xFFFE) {
      continue;  // CTAB is the first comment token after the version token
    }
    const uint32_t length = token >> 16;
    if (i + 1 + length > code.size() || length < 8 || code[i + 1] != 0x42415443 /* 'CTAB' */) {
      break;
    }
    const auto* ctab = reinterpret_cast<const uint8_t*>(&code[i + 2]);
    const size_t ctabSize = (length - 1) * 4;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(ctab + 12);
    const uint32_t infoOffset = *reinterpret_cast<const uint32_t*>(ctab + 16);
    for (uint32_t c = 0; c < count && infoOffset + (c + 1) * 20 <= ctabSize; ++c) {
      const uint8_t* info = ctab + infoOffset + c * 20;
      const uint32_t nameOffset = *reinterpret_cast<const uint32_t*>(info);
      const uint16_t registerSet = *reinterpret_cast<const uint16_t*>(info + 4);
      const uint16_t registerIndex = *reinterpret_cast<const uint16_t*>(info + 6);
      if (registerSet == 3 /* D3DXRS_SAMPLER */ && nameOffset < ctabSize) {
        samplers.push_back({ reinterpret_cast<const char*>(ctab + nameOffset), registerIndex });
      }
    }
    break;
  }
  return samplers;
}

std::string lower(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

// The material graphs name the colour sampler "diffusemap_N", "Diffuse_N" or "BaseTexture_N" (read from the
// colour-pass pixel shaders' CTAB); normal and specular maps use other names.
ShaderSamplers shaderSamplers(IDirect3DPixelShader9* shader) {
  auto it = s_shaderSamplers.find(shader);
  if (it != s_shaderSamplers.end()) {
    return it->second;
  }
  ShaderSamplers regs;
  const auto samplers = readSamplers(shader);
  std::string names;
  for (const auto& s : samplers) {
    names += s.name + "=s" + std::to_string(s.reg) + " ";
    const std::string n = lower(s.name);
    if (regs.albedo < 0 && (n.find("diffuse") != std::string::npos || n.find("basetexture") != std::string::npos)) {
      regs.albedo = static_cast<int>(s.reg);
    }
    // First normal sampler ("normalmap_0", "NormalMap_0", "Normal_0"); layered PS also have NormalLayerN.
    if (regs.normal < 0 && n.find("normal") != std::string::npos && n.find("layer") == std::string::npos) {
      regs.normal = static_cast<int>(s.reg);
    }
  }
  log::line("pixel shader %p samplers: %s-> albedo %d normal %d", shader, names.c_str(), regs.albedo, regs.normal);
  s_shaderSamplers.emplace(shader, regs);
  return regs;
}

// Saves the shader bytecode as ShaderDumpDir\<prefix>_<fnv64>.bin (once) for offline disassembly.
template <class Shader>
void dumpShader(Shader* shader, uint64_t hash, const wchar_t* prefix = L"ps") {
  if (s_shaderDumpDir.empty()) {
    return;
  }
  wchar_t path[MAX_PATH];
  swprintf_s(path, L"%s\\%s_%016llX.bin", s_shaderDumpDir.c_str(), prefix, static_cast<unsigned long long>(hash));
  UINT size = 0;
  if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES || FAILED(shader->GetFunction(nullptr, &size)) || !size) {
    return;
  }
  std::vector<uint8_t> code(size);
  if (SUCCEEDED(shader->GetFunction(code.data(), &size))) {
    if (FILE* f = _wfopen(path, L"wb")) {
      fwrite(code.data(), 1, code.size(), f);
      fclose(f);
    }
  }
}

// Specular role of a pixel shader: FNV-1a 64 of its bytecode looked up in the census table (specular_table.h).
const specular_table::Entry* shaderSpecular(IDirect3DPixelShader9* shader) {
  auto it = s_shaderSpecular.find(shader);
  if (it != s_shaderSpecular.end()) {
    return it->second;
  }
  const specular_table::Entry* found = nullptr;
  UINT size = 0;
  uint64_t hash = 0;
  if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0) {
    std::vector<uint8_t> code(size);
    if (SUCCEEDED(shader->GetFunction(code.data(), &size))) {
      hash = 0xCBF29CE484222325ull;
      for (uint8_t b : code) {
        hash = (hash ^ b) * 0x100000001B3ull;
      }
      s_shaderHash[shader] = hash;
      for (const auto& e : specular_table::kEntries) {
        if (e.bytecodeHash == hash) {
          found = &e;
          break;
        }
      }
    }
  }
  log::line("pixel shader %p bytecode %016llX: specular %s", shader, static_cast<unsigned long long>(hash),
            found ? found->name : "NOT IN CENSUS (roughness left at the default)");
  // Shaders outside the census: save their bytecode for offline analysis (disassembly + data-flow census).
  if (!found && hash) {
    dumpShader(shader, hash);
  }
  s_shaderSpecular.emplace(shader, found);
  return found;
}

IDirect3DTexture9* boundTexture(void* device, int reg) {
  if (reg < 0 || reg >= static_cast<int>(game::kDeviceTextureStages)) {
    return nullptr;
  }
  auto* base = game::field<IDirect3DBaseTexture9*>(device, game::kDeviceTextureCache + reg * 4);
  return base && base->GetType() == D3DRTYPE_TEXTURE ? static_cast<IDirect3DTexture9*>(base) : nullptr;
}

void addRefTextures(const SurfaceMaterial& m) {
  for (IDirect3DTexture9* tex : { m.albedo, m.normal, m.specTexture, m.layerAlbedo[0], m.layerAlbedo[1],
                                  m.layerNormal[0], m.layerNormal[1], m.skinMask, m.skinModulate }) {
    if (tex) {
      tex->AddRef();
    }
  }
}
// Range index of the draw entry of `meshInstance` that uses `matInst` (list 0, the colour-pass list).
bool findRange(const void* meshInstance, const void* matInst, uint32_t& range) {
  const auto* entries = game::field<const uint8_t*>(meshInstance, game::kMeshInstanceDrawEntries);
  const uint32_t count = game::field<uint32_t>(meshInstance, game::kMeshInstanceDrawEntryCount) & 0x3FFF;
  for (uint32_t e = 0; entries && e < count; ++e) {
    const uint8_t* entry = entries + e * game::kDrawEntrySize;
    if (game::field<const void*>(entry, game::kDrawEntryMaterialInstance) == matInst) {
      range = entry[game::kDrawEntryRange] & 0x7F;
      return true;
    }
  }
  return false;
}

void record(void* matInst, void* device, void* item) {
  const uint32_t passType = game::field<uint32_t>(item, game::kRenderItemPassType);
  if (passType > 1) {
    return;  // depth / shadow passes bind no colour textures
  }
  const void* meshInstance = static_geometry::currentMeshInstance();
  const void* staticMesh = nullptr;
  const void* skinnedMatInst = nullptr;
  uint32_t range = 0;
  skinned::CurrentEntry skinnedEntry;
  if (const void* dynamicMesh = dynamic_mesh::onEndDraw(matInst)) {
    // Cloth draw (DX9DynamicSubMeshInstance): keyed like skinned entries, (resource, 0, MaterialInstance).
    staticMesh = dynamicMesh;
    range = 0;
    skinnedMatInst = matInst;
  } else if (skinned::takeCurrentEntry(matInst, skinnedEntry)) {
    // Skinned draw: the palette hook recorded this entry just before its draw (0x14-byte entries).
    staticMesh = skinnedEntry.staticMesh;
    range = skinnedEntry.range;
    skinnedMatInst = matInst;
  } else {
    if (!meshInstance) {
      return;
    }
    staticMesh = game::field<const void*>(meshInstance, game::kMeshInstanceStaticMesh);
    if (!staticMesh || !findRange(meshInstance, matInst, range)) {
      return;
    }
  }

  SurfaceMaterial m;
  const void* material = game::field<const void*>(matInst, game::kMaterialInstanceMaterial);
  if (material) {
    const uint32_t flags = game::field<uint32_t>(material, game::kMaterialFlags);
    m.alphaTest = (flags & game::kMaterialAlphaTest) != 0;
    m.alphaLess = (flags & game::kMaterialAlphaFuncLess) != 0;
    m.alphaRef = static_cast<uint8_t>((flags >> 6) & 0xFF);
  }
  auto* shader = game::field<IDirect3DPixelShader9*>(device, game::kDevicePixelShaderCache);
  std::lock_guard lock { s_mutex };
  const ShaderSamplers regs = shader ? shaderSamplers(shader) : ShaderSamplers {};
  m.albedo = boundTexture(device, regs.albedo);
  m.normal = boundTexture(device, regs.normal);
  // Draws whose colour texture is a render target (Animus menu band: another view's output, which the CPU upload
  // cannot read): rasterized over the ray-traced image instead (hud.cpp).
  if (m.albedo) {
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(m.albedo->GetLevelDesc(0, &desc)) && (desc.Usage & D3DUSAGE_RENDERTARGET)) {
      std::lock_guard rt { s_rtMutex };
      s_rtMatInsts.insert(matInst);
      s_rtSurfaces.insert({ staticMesh, range });
    }
  }
  const specular_table::Entry* spec = shader ? shaderSpecular(shader) : nullptr;
  const uint64_t shaderHash = shader ? s_shaderHash[shader] : 0;
  m.shaderHash = shaderHash;
  m.hiddenEffect = shader && std::find(std::begin(game::kHiddenEffectPs), std::end(game::kHiddenEffectPs), shaderHash) !=
                                 std::end(game::kHiddenEffectPs);
  if (shader && shaderHash == game::kConstantColorPs) {
    const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                           game::kDevicePixelConstants);
    m.constantColor = true;
    for (int i = 0; i < 3; ++i) {
      m.color[i] = std::clamp(constants[game::kConstantColorRegister * 4 + i], 0.0f, 1.0f);
    }
  }
  // PS outside the census: also save the vertex shaders feeding them (the specular census classifies PS inputs from
  // the VS outputs) and log each (PS, VS) pair once.
  const bool specialShader =
      std::any_of(std::begin(game::kWaterPs), std::end(game::kWaterPs),
                  [&](const game::WaterShader& w) { return w.hash == shaderHash; }) ||
      std::find(std::begin(game::kGlassPs), std::end(game::kGlassPs), shaderHash) != std::end(game::kGlassPs);
  if (shader && (!spec || specialShader) && !s_shaderDumpDir.empty()) {
    auto* vs = game::field<IDirect3DVertexShader9*>(device, game::kDeviceVertexShaderCache);
    static std::map<std::pair<IDirect3DPixelShader9*, IDirect3DVertexShader9*>, bool> s_pairs;
    if (vs && !s_pairs[{ shader, vs }]) {
      s_pairs[{ shader, vs }] = true;
      UINT size = 0;
      uint64_t vsHash = 0xCBF29CE484222325ull;
      if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size) {
        std::vector<uint8_t> code(size);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) {
          for (uint8_t b : code) {
            vsHash = (vsHash ^ b) * 0x100000001B3ull;
          }
        }
      }
      log::line("shader pair ps %016llX vs %016llX", static_cast<unsigned long long>(shaderHash),
                static_cast<unsigned long long>(vsHash));
      dumpShader(vs, vsHash, L"vs");
    }
  }
  // Blended surfaces (material flags & 7): log each PS once and save its bytecode (the census covers opaque PS only).
  if (material && shader && (game::field<uint32_t>(material, game::kMaterialFlags) & 7) != 0) {
    static std::unordered_map<IDirect3DPixelShader9*, bool> s_blendLogged;
    if (!s_blendLogged[shader]) {
      s_blendLogged[shader] = true;
      log::line("blended PS %016llX mode %u list-pass %u samplers albedo %d normal %d", static_cast<unsigned long long>(shaderHash),
                game::field<uint32_t>(material, game::kMaterialFlags) & 7, passType, regs.albedo, regs.normal);
      dumpShader(shader, shaderHash);
    }
  }
  const game::LayeredShader* layered = nullptr;
  for (const auto& l : game::kLayeredTerrainPs) {
    if (shader && l.hash == shaderHash) layered = &l;
  }
  if (layered) {
    const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                           game::kDevicePixelConstants);
    m.layered = true;
    for (int k = 0; k < 2; ++k) {
      m.layerAlbedo[k] = boundTexture(device, game::kLayerAlbedoSampler[k]);
      m.layerNormal[k] = boundTexture(device, game::kLayerNormalSampler[k]);
    }
    for (int t = 0; t < 3; ++t) {
      for (int i = 0; i < 3; ++i) {
        m.tint[t][i] = constants[(layered->tintRegister + t) * 4 + i];
      }
    }
  }
  if (shader && std::find(std::begin(game::kGlassPs), std::end(game::kGlassPs), shaderHash) != std::end(game::kGlassPs)) {
    m.glass = true;
  }
  for (const auto& w : game::kWaterPs) {
    if (shader && w.hash == shaderHash) {
      const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                             game::kDevicePixelConstants);
      m.water = true;
      for (int i = 0; i < 3; ++i) {
        m.waterColor[i] = constants[w.colorRegister * 4 + i];  // Color_9: the water tint (remix.cpp)
      }
      m.waterOpacity = constants[w.opacityRegister * 4] *
                       std::max(constants[w.lodFactorRegister * 4], constants[w.lodToggleRegister * 4]);
      // Panner01 rows (c128/c129 .xyz) change every frame (texture scrolling): kept outside the material state so
      // the material is not re-created per frame (remix.cpp re-meshes the water with them).
      auto& rows = s_waterPanner[{ staticMesh, range }];
      for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < 3; ++i) {
          rows[r * 3 + i] = constants[(game::kWaterPannerRegister + r) * 4 + i];
        }
      }
      // The PS's Phong exponent is a constant: same translation as census kConstant shaders.
      m.specMode = static_cast<uint8_t>(1 + static_cast<int>(specular_table::Exponent::kConstant));
      m.specParam = constants[w.phongRegister * 4];
    }
  }
  for (const auto& b : game::kBlendTintPs) {
    if (shader && b.hash == shaderHash) {
      const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                             game::kDevicePixelConstants);
      const float boost = b.boostRegister >= 0 ? std::max(1.0f, constants[b.boostRegister * 4]) : 1.0f;
      m.blendTinted = true;
      m.blendVertexColor = b.vertexColor;
      for (int i = 0; i < 3; ++i) {
        m.blendColor[i] = constants[b.colorRegister * 4 + i] * boost;
      }
      m.blendAlpha = b.alphaRegister >= 0 ? constants[b.alphaRegister * 4] : 1.0f;
    }
  }
  if (shader && std::find(std::begin(game::kSkinPs), std::end(game::kSkinPs), shaderHash) != std::end(game::kSkinPs)) {
    const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                           game::kDevicePixelConstants);
    m.skin = true;
    m.albedo = boundTexture(device, game::kSkinBaseSampler);
    m.normal = boundTexture(device, game::kSkinNormalSampler);
    m.skinMask = boundTexture(device, game::kSkinMaskSampler);
    m.skinModulate = boundTexture(device, game::kSkinModulateSampler);
    for (int i = 0; i < 3; ++i) {
      m.skinTint[i] = constants[game::kSkinTintRegister * 4 + i];
      m.skinBias[i] = constants[game::kSkinBiasRegister * 4 + i];
    }
    // Measured (AC1RTX.log 2026-10-01 18:38): characters sharing one MaterialInstance draw it with different
    // c128/c129 (per-character skin tone); keyed by MaterialInstance alone, the shared material was re-created at
    // every draw (2M materials in 15 min, characters grey in between). Key = MaterialInstance x tone hash.
    if (skinnedMatInst) {
      uint64_t tone = 0xCBF29CE484222325ull;
      for (const float* f : { m.skinTint, m.skinBias }) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(f);
        for (size_t b = 0; b < 3 * sizeof(float); ++b) {
          tone = (tone ^ bytes[b]) * 0x100000001B3ull;
        }
      }
      const auto* variantKey = reinterpret_cast<const void*>(
          reinterpret_cast<uintptr_t>(skinnedMatInst) ^ static_cast<uintptr_t>((tone >> 32) ^ tone) << 4);
      skinned::setLastDrawKey(skinnedMatInst, variantKey);
      skinnedMatInst = variantKey;
    }
  }
  if (spec) {
    m.specMode = static_cast<uint8_t>(1 + static_cast<int>(spec->mode));
    const auto* constants = reinterpret_cast<const float*>(static_cast<const uint8_t*>(device) +
                                                           game::kDevicePixelConstants);
    switch (spec->mode) {
      case specular_table::Exponent::kNone:
        break;
      case specular_table::Exponent::kConstant:
        m.specParam = constants[spec->constReg * 4 + spec->constComp];
        break;
      case specular_table::Exponent::kTexturePlusConstant:
        m.specParam = constants[spec->constReg * 4 + spec->constComp];
        m.specTexture = boundTexture(device, spec->sampler);
        m.specChannel = spec->channel;
        break;
      case specular_table::Exponent::kTextureTimesK:
        m.specParam = spec->k;
        m.specTexture = boundTexture(device, spec->sampler);
        m.specChannel = spec->channel;
        break;
    }
  }

  const SurfaceKey key { staticMesh, range, skinnedMatInst };
  auto it = s_current.find(key);
  // Measured: the colour pass draws some surfaces twice per frame with the same albedo, once with a normal-mapped
  // PS (diffusemap_1 + normalmap_0) and once with a diffuse-only PS (diffusemap_0). Keep the richer state instead of
  // letting the second draw drop the normal map (which re-created the Remix material twice per frame).
  if (it != s_current.end() && it->second.albedo == m.albedo && !m.normal && it->second.normal) {
    m.normal = it->second.normal;
    m.specMode = it->second.specMode;  // the richer (normal-mapped) draw also carries the specular term
    m.specTexture = it->second.specTexture;
    m.specChannel = it->second.specChannel;
    m.specParam = it->second.specParam;
    m.layered = it->second.layered;
    std::memcpy(m.layerAlbedo, it->second.layerAlbedo, sizeof(m.layerAlbedo));
    std::memcpy(m.layerNormal, it->second.layerNormal, sizeof(m.layerNormal));
    std::memcpy(m.tint, it->second.tint, sizeof(m.tint));
  }
  // Measured (AC1RTX.log 2026-09-30 23:40): skinned surfaces are also drawn by rim-light PS outside the census
  // (447118CA: pow(|dot(V, N)|, RimPower_16) over DiffuseMap, no Phong term) alternating every frame with the
  // colour PS that carries the specular (ec408913); a PS outside the census holds no specular data, so the
  // specular read from the census PS is kept (otherwise the material was re-created twice per frame).
  // Extended census (dumped PS): the rim PS is now known as "no Phong lobe" (kNone) - it still carries no specular
  // information about the surface, so it keeps a Phong-derived specular as well.
  const uint8_t kNoneMode = 1 + static_cast<uint8_t>(specular_table::Exponent::kNone);
  // Measured (2026-10-01: 34k specMode changes, almost all skinned): the same surface is also
  // drawn by two census PS with different Phong exponent forms (light-count variants), alternating every frame.
  // The first Phong reading of a surface is kept while its albedo is unchanged. Also (still 20k skinned specMode
  // changes): mode 0 (PS outside the table, e.g. the hair PS) alternating with mode 1 (rim PS, no
  // Phong): neither carries a Phong term, so the recorded one is kept. Only a Phong reading replaces a non-Phong one.
  const bool recordedPhong = it != s_current.end() && it->second.specMode > kNoneMode;
  const bool newPhong = m.specMode > kNoneMode;
  if (it != s_current.end() && !m.water && it->second.albedo == m.albedo && !(newPhong && !recordedPhong) &&
      (m.specMode != it->second.specMode || m.specTexture != it->second.specTexture ||
       m.specParam != it->second.specParam || m.specChannel != it->second.specChannel)) {
    m.specMode = it->second.specMode;
    m.specTexture = it->second.specTexture;
    m.specChannel = it->second.specChannel;
    m.specParam = it->second.specParam;
  }
  // Same for the skin composite: other PS drawing the same surface (rim-light passes) bind the same base texture
  // but carry no skin mask/tint, so the skin data read from the skin PS is kept.
  // Same for glass (26k glass changes in the Animus room): the glass surface is also drawn by a non-glass PS.
  if (it != s_current.end() && !m.glass && it->second.glass && it->second.albedo == m.albedo) {
    m.glass = true;
  }
  if (it != s_current.end() && !m.blendTinted && it->second.blendTinted && it->second.albedo == m.albedo) {
    m.blendTinted = true;
    m.blendVertexColor = it->second.blendVertexColor;
    std::memcpy(m.blendColor, it->second.blendColor, sizeof(m.blendColor));
    m.blendAlpha = it->second.blendAlpha;
  }
  if (it != s_current.end() && !m.skin && it->second.skin && it->second.albedo == m.albedo) {
    m.skin = true;
    m.skinMask = it->second.skinMask;
    m.skinModulate = it->second.skinModulate;
    std::memcpy(m.skinTint, it->second.skinTint, sizeof(m.skinTint));
    std::memcpy(m.skinBias, it->second.skinBias, sizeof(m.skinBias));
    m.normal = m.normal ? m.normal : it->second.normal;
  }
  if (it != s_current.end() && it->second == m) {
    return;
  }
  addRefTextures(m);  // keep the pointers valid while a Remix material may still upload them
  if (it != s_current.end()) {
    releaseTextures(it->second);
    it->second = m;
  } else {
    s_current.emplace(key, m);
  }
  addRefTextures(m);  // references owned by the queued update
  s_updates.push_back({ staticMesh, range, skinnedMatInst, m });
}

void __fastcall endDrawHook(void* matInst, void* /*edx*/, uint32_t recordIndex, void* device, void* item) {
  if (matInst && device && item) {
    record(matInst, device, item);
  }
  s_originalEndDraw(matInst, recordIndex, device, item);
}

} // namespace

void setShaderDumpDir(const std::wstring& dir) {
  s_shaderDumpDir = dir;
  if (!dir.empty()) {
    CreateDirectoryW(dir.c_str(), nullptr);
  }
}

bool install() {
  return hook::install("MaterialInstance_EndDraw", game::kMaterialEndDraw, game::kMaterialEndDrawPrologue,
                       reinterpret_cast<void*>(&endDrawHook), &s_originalEndDraw);
}

std::vector<Update> takeUpdates() {
  std::lock_guard lock { s_mutex };
  std::vector<Update> out = std::move(s_updates);
  s_updates.clear();
  return out;
}

void releaseTextures(const SurfaceMaterial& m) {
  for (IDirect3DTexture9* tex : { m.albedo, m.normal, m.specTexture, m.layerAlbedo[0], m.layerAlbedo[1],
                                  m.layerNormal[0], m.layerNormal[1], m.skinMask, m.skinModulate }) {
    if (tex) {
      tex->Release();
    }
  }
}

uint32_t heldReferences(IDirect3DTexture9* tex) {
  std::lock_guard lock { s_mutex };
  uint32_t count = 0;
  auto countIn = [&](const SurfaceMaterial& m) {
    // The recorded state holds one reference per field, plus one per queued update (see record()).
    count += (m.albedo == tex ? 1 : 0) + (m.normal == tex ? 1 : 0) + (m.specTexture == tex ? 1 : 0) +
             (m.layerAlbedo[0] == tex ? 1 : 0) + (m.layerAlbedo[1] == tex ? 1 : 0) +
             (m.layerNormal[0] == tex ? 1 : 0) + (m.layerNormal[1] == tex ? 1 : 0) + (m.skinMask == tex ? 1 : 0) +
             (m.skinModulate == tex ? 1 : 0);
  };
  for (const auto& [key, m] : s_current) {
    countIn(m);
  }
  for (const auto& u : s_updates) {
    countIn(u.material);
  }
  return count;
}

bool drawsRenderTarget(const void* matInst) {
  std::lock_guard rt { s_rtMutex };
  return s_rtMatInsts.count(matInst) != 0;
}

bool isRenderTargetSurface(const void* staticMesh, uint32_t range) {
  std::lock_guard rt { s_rtMutex };
  return s_rtSurfaces.count({ staticMesh, range }) != 0;
}

bool waterPanner(const void* staticMesh, uint32_t range, float rows[2][3]) {
  std::lock_guard lock { s_mutex };
  auto it = s_waterPanner.find({ staticMesh, range });
  if (it == s_waterPanner.end()) {
    return false;
  }
  for (int r = 0; r < 2; ++r) {
    for (int i = 0; i < 3; ++i) {
      rows[r][i] = it->second[r * 3 + i];
    }
  }
  return true;
}

void forgetMesh(const void* staticMesh) {
  std::lock_guard lock { s_mutex };
  for (auto w = s_waterPanner.lower_bound({ staticMesh, 0u });
       w != s_waterPanner.end() && w->first.first == staticMesh;) {
    w = s_waterPanner.erase(w);
  }
  auto it = s_current.lower_bound({ staticMesh, 0u, nullptr });
  while (it != s_current.end() && std::get<0>(it->first) == staticMesh) {
    releaseTextures(it->second);
    it = s_current.erase(it);
  }
}

} // namespace ac1rtx::materials
