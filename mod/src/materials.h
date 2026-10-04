#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct IDirect3DTexture9;

namespace ac1rtx::materials {

// What the engine's colour pass bound for one primitive range, read at draw time.
struct SurfaceMaterial {
  IDirect3DTexture9* albedo = nullptr;  // AddRef'd while referenced by the mod; may be null (untextured)
  IDirect3DTexture9* normal = nullptr;  // tangent-space normal map (engine decode: see game.h), AddRef'd; may be null
  bool alphaTest = false;
  bool alphaLess = false;               // ALPHAFUNC LESS instead of GREATEREQUAL
  uint8_t alphaRef = 0;
  // Specular (Phong exponent n of the bound PS, specular_table.h). specMode: 0 = PS not in the census,
  // otherwise 1 + specular_table::Exponent (1 none, 2 constant, 3 texture + constant, 4 texture * k).
  uint8_t specMode = 0;
  IDirect3DTexture9* specTexture = nullptr;  // texture modes, AddRef'd like the others
  uint8_t specChannel = 0;
  float specParam = 0.0f;               // constant modes: the register value; kTextureTimesK: k
  // Layered terrain PS (game.h kLayeredTerrain*): texture layers 1/2 (albedo, normal) and the tints
  // BaseColor / Layer1Color / Layer2Color (rgb) read from the PS constant shadow at the draw.
  bool layered = false;
  IDirect3DTexture9* layerAlbedo[2] = {};
  IDirect3DTexture9* layerNormal[2] = {};
  float tint[3][3] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
  // Skin PS (game.h kSkinPs): albedo = lerp(albedo, skinTint, skinMask.r) * saturate(skinModulate + skinBias).
  bool skin = false;
  IDirect3DTexture9* skinMask = nullptr;
  IDirect3DTexture9* skinModulate = nullptr;
  float skinTint[3] = {};
  float skinBias[3] = {};

  // Blended PS with a constant tint (game.h kBlendTintPs): rgb factor (boost already applied, may exceed 1), alpha
  // factor, and whether COLOR0 multiplies the result.
  bool blendTinted = false;
  bool blendVertexColor = true;
  float blendColor[3] = { 1, 1, 1 };
  float blendAlpha = 1.0f;

  // Water PS (game.h kWaterPs): constant colour and opacity instead of an albedo texture.
  bool water = false;
  float waterColor[3] = {};
  float waterOpacity = 1.0f;

  // Glass PS (game.h kGlassPs): thin-walled translucent (remix.cpp).
  bool glass = false;

  // Untextured PS with a constant colour (game.h kConstantColorPs): Remix albedo constant.
  bool constantColor = false;
  float color[3] = {};

  bool hiddenEffect = false;  // game.h kHiddenEffectPs
  uint64_t shaderHash = 0;  // colour-pass PS of the last capture (diagnostics only, not compared)

  bool sameSkin(const SurfaceMaterial& o) const {
    return skin == o.skin && skinMask == o.skinMask && skinModulate == o.skinModulate &&
           std::memcmp(skinTint, o.skinTint, sizeof(skinTint)) == 0 &&
           std::memcmp(skinBias, o.skinBias, sizeof(skinBias)) == 0;
  }
  bool operator==(const SurfaceMaterial& o) const {
    return albedo == o.albedo && normal == o.normal && alphaTest == o.alphaTest && alphaLess == o.alphaLess &&
           alphaRef == o.alphaRef && specMode == o.specMode && specTexture == o.specTexture &&
           specChannel == o.specChannel && specParam == o.specParam && layered == o.layered &&
           layerAlbedo[0] == o.layerAlbedo[0] && layerAlbedo[1] == o.layerAlbedo[1] &&
           layerNormal[0] == o.layerNormal[0] && layerNormal[1] == o.layerNormal[1] &&
           std::memcmp(tint, o.tint, sizeof(tint)) == 0 && sameSkin(o) && blendTinted == o.blendTinted &&
           blendVertexColor == o.blendVertexColor && std::memcmp(blendColor, o.blendColor, sizeof(blendColor)) == 0 &&
           blendAlpha == o.blendAlpha && water == o.water && constantColor == o.constantColor && hiddenEffect == o.hiddenEffect &&
           std::memcmp(color, o.color, sizeof(color)) == 0 &&
           std::memcmp(waterColor, o.waterColor, sizeof(waterColor)) == 0 && waterOpacity == o.waterOpacity &&
           glass == o.glass;
  }
};

struct Update {
  const void* staticMesh = nullptr;     // DX9StaticMesh*
  uint32_t range = 0;                   // primitive range index
  // Skinned draws: the entry's MaterialInstance (instances sharing a skinned mesh use different ones). Static
  // draws: nullptr (measured live: no static (mesh, range) is drawn with more than one MaterialInstance).
  const void* matInst = nullptr;
  SurfaceMaterial material;
};

// Folder where pixel shaders outside the specular census are saved (ps_<fnv64>.bin); empty disables.
void setShaderDumpDir(const std::wstring& dir);

// Hooks MaterialInstance_EndDraw. Must be installed after static_geometry (uses its current-instance tracking).
bool install();

// Surface materials first seen or changed since the previous call. Render thread only.
// Each update's textures carry one reference each that the consumer must release (releaseTextures).
std::vector<Update> takeUpdates();

// Releases the texture references held by m.
void releaseTextures(const SurfaceMaterial& m);

// Number of references to `tex` held by the recorded surface states and pending updates.
uint32_t heldReferences(IDirect3DTexture9* tex);

// True if this MaterialInstance's colour-pass draw was seen binding a render-target colour texture (a surface showing
// another view's output, e.g. the Animus menu band): such a draw cannot be uploaded and is rasterized instead.
bool drawsRenderTarget(const void* matInst);
bool isRenderTargetSurface(const void* staticMesh, uint32_t range);  // same, by static surface

// Latest water texture scroll (Panner01 rows, c128/c129 .xyz) recorded for a (mesh, range); false if none.
bool waterPanner(const void* staticMesh, uint32_t range, float rows[2][3]);

// Drops the recorded state of a destroyed DX9StaticMesh.
void forgetMesh(const void* staticMesh);

} // namespace ac1rtx::materials
