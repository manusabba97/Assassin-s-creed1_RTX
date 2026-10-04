#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// PBR material overrides made offline (ComfyUI workflow concept_flussoPBR_AC1remix, tools/pbr_batch.py).
// Engine textures are identified across sessions by the FNV-1a 64 hash of their level-0 bytes (the D3D9 pointer
// changes every run). Layout under Settings::folder:
//   dump\<hash>.png                                  engine albedo exported by the mod (Dump=1)
//   maps\<hash>\<variant>\<hash>_<map>_NNNNN.png     workflow output (variant x4 / x2), map = albedo/normal/height/
//                                                    roughness/metallic
//   maps\<hash>\<variant>\<hash>_material_NNNNN.json "remix_height_strength" (metres of relief)
// The newest NNNNN of each map is used.
namespace ac1rtx::pbr {

struct Settings {
  bool enable = false;  // use the maps found under maps\ (otherwise the engine textures as before)
  bool dump = false;    // export the layered terrain albedo textures to dump\ (once per texture)
  bool layerPom = false;  // test: height/POM also on the terrain layer overlays (alpha-blended decals)
  bool exportVegetation = false;  // write vegetation meshes, textures and placements to <folder>\vegetation
  std::wstring folder;
  std::wstring variant = L"x4";  // maps sub-folder: the workflow upscale factor
};

void configure(const Settings& settings);
bool enabled();
bool dumpEnabled();
bool exportVegetation();
// PNG from tightly packed RGB8 (+ optional alpha, written as 32-bit when some texel is not opaque).
bool writePng(const std::wstring& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb,
              const std::vector<uint8_t>* alpha = nullptr);
bool layerPom();
const std::wstring& folder();
const std::wstring& variant();

uint64_t fnv1a(const uint8_t* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

// 8-bit RGBA image, rows top to bottom.
struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool empty() const { return rgba.empty(); }
};

// Block-compressed map from tools/pbr_pack.py (<hash>_<map>.ac1t): "AC1T", uint32 remixapi_Format, width, height,
// mip levels, then the mip chain. Uploaded as it is: nothing is decoded in the game process.
struct Packed {
  uint32_t format = 0, width = 0, height = 0, levels = 0;
  std::vector<uint8_t> data;
  bool empty() const { return data.empty(); }
};

enum Map { kAlbedo, kNormal, kRoughness, kMetallic, kHeight, kMapCount };

struct Override {
  Image albedo, normal, roughness, metallic, height;  // PNG maps (used only when no packed file exists)
  Packed packed[kMapCount];
  bool isPacked = false;
  float heightMetres = 0.0f;  // VLM's suggested relief depth (material JSON), 0 when missing
  // tools/pbr_pack.py tune() (material JSON): relief height as a fraction of one texture repeat (0 = use heightMetres)
  // and whether POM is on by default (off for cloth, leather, metal, foliage, alpha, small or flat textures).
  float reliefUv = 0.0f;
  bool pomDefault = true;
  float metallicConstant = 0.0f;  // material class "metal" (weathered iron, bronze): used when there is no metallic map
};

// Writes dump\<hash><suffix>.png from tightly packed RGB8 (skipped when the file already exists). The engine normal
// map of an albedo is dumped as <albedo hash>_normal.png (raw engine RGB, see remix.cpp encodeNormalTexel).
// alpha (optional, one byte per texel): written as a 32-bit PNG when some texel is not opaque.
void dump(uint64_t hash, const wchar_t* suffix, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb,
          const std::vector<uint8_t>* alpha = nullptr);

// Loads the newest maps for `hash`, or nullptr when there is no albedo for it. Images can be released by the caller
// after upload.
std::unique_ptr<Override> load(uint64_t hash);
// One packed map file (<hash>_<map>.ac1t); false when missing or malformed.
bool loadPackedFile(const std::wstring& path, Packed& out);

} // namespace ac1rtx::pbr
