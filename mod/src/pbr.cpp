#include "pbr.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>

// gdiplus.h uses unqualified min/max, which NOMINMAX removes from windows.h.
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>

namespace ac1rtx::pbr {

namespace {

Settings s_settings;
ULONG_PTR s_gdiplusToken = 0;
bool s_gdiplus = false;

// Image/png encoder CLSID of GDI+ (same as screenshot.cpp).
const CLSID kPngEncoder = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };

bool ensureGdiplus() {
  static std::once_flag once;
  std::call_once(once, [] {
    Gdiplus::GdiplusStartupInput input;
    s_gdiplus = Gdiplus::GdiplusStartup(&s_gdiplusToken, &input, nullptr) == Gdiplus::Ok;
    if (!s_gdiplus) {
      log::line("pbr: GdiplusStartup failed");
    }
  });
  return s_gdiplus;
}

std::wstring hashName(uint64_t hash) {
  wchar_t buf[20];
  swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(hash));
  return buf;
}

bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Newest "<prefix>_NNNNN.<ext>" in `dir` (largest NNNNN), empty when none.
std::wstring newest(const std::wstring& dir, const std::wstring& prefix, const wchar_t* ext) {
  WIN32_FIND_DATAW data;
  const std::wstring pattern = dir + L"\\" + prefix + L"_*." + ext;
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::wstring best;
  long bestIndex = -1;
  do {
    const std::wstring name = data.cFileName;
    const size_t start = prefix.size() + 1;
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot <= start) {
      continue;
    }
    const std::wstring digits = name.substr(start, dot - start);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), iswdigit)) {
      continue;  // e.g. "<hash>_albedo_00001" must not match prefix "<hash>_albedo_x"
    }
    const long index = std::wcstol(digits.c_str(), nullptr, 10);
    if (index > bestIndex) {
      bestIndex = index;
      best = dir + L"\\" + name;
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return best;
}

bool loadPng(const std::wstring& path, Image& out) {
  if (path.empty() || !ensureGdiplus()) {
    return false;
  }
  Gdiplus::Bitmap bitmap(path.c_str());
  if (bitmap.GetLastStatus() != Gdiplus::Ok) {
    log::line("pbr: cannot read %ls", path.c_str());
    return false;
  }
  const UINT w = bitmap.GetWidth(), h = bitmap.GetHeight();
  Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
  Gdiplus::BitmapData data = {};
  if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
    return false;
  }
  out.width = w;
  out.height = h;
  out.rgba.resize(size_t(w) * h * 4);
  for (UINT y = 0; y < h; ++y) {
    const auto* row = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
    for (UINT x = 0; x < w; ++x) {
      const uint8_t* p = row + x * 4;  // GDI+ 32bppARGB = B, G, R, A in memory
      uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
      o[0] = p[2];
      o[1] = p[1];
      o[2] = p[0];
      o[3] = p[3];
    }
  }
  bitmap.UnlockBits(&data);
  return true;
}

bool loadPacked(const std::wstring& path, Packed& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) {
    return false;
  }
  char magic[4] = {};
  uint32_t header[4] = {};
  bool ok = fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "AC1T", 4) == 0 && fread(header, 4, 4, f) == 4;
  if (ok) {
    out.format = header[0];
    out.width = header[1];
    out.height = header[2];
    out.levels = header[3];
    fseek(f, 0, SEEK_END);
    const long size = ftell(f) - 20;
    fseek(f, 20, SEEK_SET);
    out.data.resize(size > 0 ? size_t(size) : 0);
    ok = size > 0 && fread(out.data.data(), 1, out.data.size(), f) == out.data.size();
  }
  fclose(f);
  if (!ok) {
    log::line("pbr: bad packed file %ls", path.c_str());
    out = {};
  }
  return ok;
}

// Material JSON of the workflow run (tools/pbr_pack.py adds class, relief_uv, pom_default).
void readMaterialJson(const std::wstring& path, Override& o) {
  if (path.empty()) {
    return;
  }
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) {
    return;
  }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    text.append(buf, n);
  }
  fclose(f);
  std::smatch match;
  if (std::regex_search(text, match, std::regex("\"remix_height_strength\"\\s*:\\s*([-+0-9.eE]+)"))) {
    o.heightMetres = std::stof(match[1].str());
  }
  if (std::regex_search(text, match, std::regex("\"relief_uv\"\\s*:\\s*([-+0-9.eE]+)"))) {
    o.reliefUv = std::stof(match[1].str());
  }
  if (std::regex_search(text, match, std::regex("\"pom_default\"\\s*:\\s*(true|false)"))) {
    o.pomDefault = match[1].str() == "true";
  }
  if (std::regex_search(text, match, std::regex("\"metallic_constant\"\\s*:\\s*([-+0-9.eE]+)"))) {
    o.metallicConstant = std::stof(match[1].str());
  }
}

} // namespace

bool loadPackedFile(const std::wstring& path, Packed& out) {
  return loadPacked(path, out);
}

void configure(const Settings& settings) {
  s_settings = settings;
  if (s_settings.dump) {
    CreateDirectoryW(s_settings.folder.c_str(), nullptr);
    CreateDirectoryW((s_settings.folder + L"\\dump").c_str(), nullptr);
  }
  log::line("pbr: enable %d dump %d folder %ls", s_settings.enable ? 1 : 0, s_settings.dump ? 1 : 0,
            s_settings.folder.c_str());
}

bool enabled() {
  return s_settings.enable && !s_settings.folder.empty();
}

bool dumpEnabled() {
  return s_settings.dump && !s_settings.folder.empty();
}

bool exportVegetation() {
  return s_settings.exportVegetation && !s_settings.folder.empty();
}

bool layerPom() {
  return s_settings.layerPom;
}

const std::wstring& folder() {
  return s_settings.folder;
}

const std::wstring& variant() {
  return s_settings.variant;
}

uint64_t fnv1a(const uint8_t* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h = (h ^ data[i]) * 0x100000001B3ull;
  }
  return h;
}

void dump(uint64_t hash, const wchar_t* suffix, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb,
          const std::vector<uint8_t>* alpha) {
  if (!dumpEnabled()) {
    return;
  }
  const std::wstring path = s_settings.folder + L"\\dump\\" + hashName(hash) + suffix + L".png";
  if (exists(path)) {
    return;
  }
  const bool ok = writePng(path, width, height, rgb, alpha);
  log::line("pbr: dump %ls (%ux%u) %s", path.c_str(), width, height, ok ? "saved" : "FAILED");
}

bool writePng(const std::wstring& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb,
              const std::vector<uint8_t>* alpha) {
  if (!ensureGdiplus()) {
    return false;
  }
  bool hasAlpha = false;
  if (alpha && alpha->size() == size_t(width) * height) {
    for (uint8_t a : *alpha) {
      if (a != 255) {
        hasAlpha = true;
        break;
      }
    }
  }
  const Gdiplus::PixelFormat format = hasAlpha ? PixelFormat32bppARGB : PixelFormat24bppRGB;
  const uint32_t bpp = hasAlpha ? 4 : 3;
  Gdiplus::Bitmap bitmap(static_cast<INT>(width), static_cast<INT>(height), format);
  Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
  Gdiplus::BitmapData data = {};
  if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeWrite, format, &data) != Gdiplus::Ok) {
    return false;
  }
  for (uint32_t y = 0; y < height; ++y) {
    auto* row = static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
    for (uint32_t x = 0; x < width; ++x) {
      const size_t i = size_t(y) * width + x;
      const uint8_t* s = &rgb[i * 3];
      row[x * bpp] = s[2];  // B, G, R(, A) in memory
      row[x * bpp + 1] = s[1];
      row[x * bpp + 2] = s[0];
      if (hasAlpha) {
        row[x * bpp + 3] = (*alpha)[i];
      }
    }
  }
  bitmap.UnlockBits(&data);
  return bitmap.Save(path.c_str(), &kPngEncoder, nullptr) == Gdiplus::Ok;
}

std::unique_ptr<Override> load(uint64_t hash) {
  if (!enabled()) {
    return nullptr;
  }
  const std::wstring name = hashName(hash);
  const std::wstring dir = s_settings.folder + L"\\maps\\" + name + L"\\" + s_settings.variant;
  auto o = std::make_unique<Override>();
  readMaterialJson(newest(dir, name + L"_material", L"json"), *o);
  static const wchar_t* kNames[kMapCount] = { L"albedo", L"normal", L"roughness", L"metallic", L"height" };
  if (loadPacked(dir + L"\\" + name + L"_albedo.ac1t", o->packed[kAlbedo])) {
    o->isPacked = true;
    for (int m = 1; m < kMapCount; ++m) {
      loadPacked(dir + L"\\" + name + L"_" + kNames[m] + L".ac1t", o->packed[m]);
    }
    log::line("pbr: %ls packed albedo %ux%u (%u mips) normal %d roughness %d metallic %d height %d (%.3f m)",
              name.c_str(), o->packed[kAlbedo].width, o->packed[kAlbedo].height, o->packed[kAlbedo].levels,
              o->packed[kNormal].empty() ? 0 : 1, o->packed[kRoughness].empty() ? 0 : 1,
              o->packed[kMetallic].empty() ? 0 : 1, o->packed[kHeight].empty() ? 0 : 1, o->heightMetres);
    return o;
  }
  if (!loadPng(newest(dir, name + L"_albedo", L"png"), o->albedo)) {
    return nullptr;
  }
  loadPng(newest(dir, name + L"_normal", L"png"), o->normal);
  loadPng(newest(dir, name + L"_roughness", L"png"), o->roughness);
  loadPng(newest(dir, name + L"_metallic", L"png"), o->metallic);
  loadPng(newest(dir, name + L"_height", L"png"), o->height);
  log::line("pbr: %ls albedo %ux%u normal %d roughness %d metallic %d height %d (%.3f m)", name.c_str(),
            o->albedo.width, o->albedo.height, o->normal.empty() ? 0 : 1, o->roughness.empty() ? 0 : 1,
            o->metallic.empty() ? 0 : 1, o->height.empty() ? 0 : 1, o->heightMetres);
  return o;
}

} // namespace ac1rtx::pbr
