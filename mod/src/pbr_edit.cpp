#include "pbr_edit.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

// gdiplus.h uses unqualified min/max, which NOMINMAX removes from windows.h.
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>

namespace ac1rtx::pbr {

namespace {

constexpr uint32_t kFormatBC1 = 133, kFormatBC3 = 135, kFormatBC5 = 139;  // remixapi_Format (tools/pbr_pack.py)
const char* kKeys[kMapCount] = { "albedo", "normal", "roughness", "metallic", "height" };

std::wstring hashName(uint64_t hash) {
  wchar_t buf[20];
  swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(hash));
  return buf;
}

bool number(const std::string& json, const std::string& key, float& out) {
  std::smatch m;
  if (std::regex_search(json, m, std::regex("\"" + key + "\"\\s*:\\s*([-+0-9.eE]+|true|false)"))) {
    const std::string v = m[1].str();
    out = v == "true" ? 1.f : v == "false" ? 0.f : std::stof(v);
    return true;
  }
  return false;
}

// ---- block codecs (formats written by tools/pbr_pack.py)

void decodeBC4(const uint8_t* b, uint8_t out[16]) {
  int pal[8] = { b[0], b[1] };
  if (b[0] > b[1]) {
    for (int i = 1; i <= 6; ++i) pal[i + 1] = ((7 - i) * b[0] + i * b[1]) / 7;
  } else {
    for (int i = 1; i <= 4; ++i) pal[i + 1] = ((5 - i) * b[0] + i * b[1]) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = static_cast<uint8_t>(pal[(bits >> (3 * t)) & 7]);
}

void encodeBC4(const uint8_t v[16], uint8_t out[8]) {
  uint8_t lo = 255, hi = 0;
  for (int t = 0; t < 16; ++t) {
    lo = std::min(lo, v[t]);
    hi = std::max(hi, v[t]);
  }
  out[0] = hi;
  out[1] = lo;
  uint64_t idx = 0;
  if (hi != lo) {
    int pal[8] = { hi, lo };
    for (int i = 1; i <= 6; ++i) pal[i + 1] = ((7 - i) * hi + i * lo) / 7;
    for (int t = 0; t < 16; ++t) {
      int best = 0, err = 1 << 30;
      for (int p = 0; p < 8; ++p) {
        const int e = std::abs(pal[p] - v[t]);
        if (e < err) {
          err = e;
          best = p;
        }
      }
      idx |= uint64_t(best) << (3 * t);
    }
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>(idx >> (8 * i));
}

void decodeColor(const uint8_t* b, bool bc1, uint8_t rgb[16][3]) {
  const uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  int pal[4][3];
  auto expand = [](uint16_t c, int o[3]) {
    o[0] = ((c >> 11) & 31) * 255 / 31;
    o[1] = ((c >> 5) & 63) * 255 / 63;
    o[2] = (c & 31) * 255 / 31;
  };
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  for (int i = 0; i < 3; ++i) {
    if (!bc1 || c0 > c1) {
      pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3;
      pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3;
    } else {
      pal[2][i] = (pal[0][i] + pal[1][i]) / 2;
      pal[3][i] = 0;
    }
  }
  const uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
  for (int t = 0; t < 16; ++t)
    for (int i = 0; i < 3; ++i) rgb[t][i] = static_cast<uint8_t>(pal[(bits >> (2 * t)) & 3][i]);
}

uint16_t to565(const float c[3]) {
  auto q = [](float v, int max) { return static_cast<uint16_t>(std::clamp(std::lround(v / 255.f * max), 0L, long(max))); };
  return static_cast<uint16_t>((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

// 4-colour block (c0 > c1): endpoints at the ends of the block's bounding box diagonal.
void encodeColor(const uint8_t rgb[16][3], uint8_t out[8]) {
  float lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
  for (int t = 0; t < 16; ++t)
    for (int i = 0; i < 3; ++i) {
      lo[i] = std::min(lo[i], float(rgb[t][i]));
      hi[i] = std::max(hi[i], float(rgb[t][i]));
    }
  uint16_t c0 = to565(hi), c1 = to565(lo);
  if (c0 < c1) std::swap(c0, c1);
  uint32_t bits = 0;
  if (c0 != c1) {
    uint8_t pal[4][3];
    uint8_t b[8] = { uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8) };
    uint8_t dummy[16][3];
    decodeColor(b, false, dummy);  // palette endpoints as the decoder sees them
    int p[4][3];
    auto expand = [](uint16_t c, int o[3]) {
      o[0] = ((c >> 11) & 31) * 255 / 31;
      o[1] = ((c >> 5) & 63) * 255 / 63;
      o[2] = (c & 31) * 255 / 31;
    };
    expand(c0, p[0]);
    expand(c1, p[1]);
    for (int i = 0; i < 3; ++i) {
      p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
      p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
    }
    for (int k = 0; k < 4; ++k)
      for (int i = 0; i < 3; ++i) pal[k][i] = static_cast<uint8_t>(p[k][i]);
    for (int t = 0; t < 16; ++t) {
      int best = 0, err = 1 << 30;
      for (int k = 0; k < 4; ++k) {
        int e = 0;
        for (int i = 0; i < 3; ++i) e += (pal[k][i] - rgb[t][i]) * (pal[k][i] - rgb[t][i]);
        if (e < err) {
          err = e;
          best = k;
        }
      }
      bits |= uint32_t(best) << (2 * t);
    }
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  std::memcpy(out + 4, &bits, 4);
}

// ---- float images

struct Img {
  uint32_t w = 0, h = 0, c = 0;
  std::vector<float> px;  // [0, 1] (normals: unit vectors, xyz)
  float* at(uint32_t x, uint32_t y) { return &px[(size_t(y) * w + x) * c]; }
};

uint32_t blocksOf(uint32_t v) { return (v + 3) / 4; }

Img decodeLevel0(const Packed& p, int map) {
  Img img;
  img.w = p.width;
  img.h = p.height;
  const uint32_t bw = blocksOf(p.width), bh = blocksOf(p.height);
  const bool colour = p.format == kFormatBC1 || p.format == kFormatBC3;
  img.c = colour ? 4 : (map == kNormal ? 3 : 1);
  img.px.assign(size_t(img.w) * img.h * img.c, 0.f);
  const size_t blockBytes = p.format == kFormatBC1 ? 8 : 16;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = p.data.data() + (size_t(by) * bw + bx) * blockBytes;
      float texel[16][4] = {};
      if (colour) {
        uint8_t rgb[16][3], a[16];
        std::fill(a, a + 16, uint8_t(255));
        if (p.format == kFormatBC3) {
          decodeBC4(b, a);
          decodeColor(b + 8, false, rgb);
        } else {
          decodeColor(b, true, rgb);
        }
        for (int t = 0; t < 16; ++t) {
          for (int i = 0; i < 3; ++i) texel[t][i] = rgb[t][i] / 255.f;
          texel[t][3] = a[t] / 255.f;
        }
      } else {
        uint8_t r[16], g[16];
        decodeBC4(b, r);
        decodeBC4(b + 8, g);
        for (int t = 0; t < 16; ++t) {
          if (map == kNormal) {
            // Remix unsigned octahedral (tools/pbr_pack.py octahedral()) -> unit vector, OpenGL green
            const float ex = r[t] / 255.f * 2.f - 1.f, ey = g[t] / 255.f * 2.f - 1.f;
            const float px = (ex + ey) * 0.5f, py = (ex - ey) * 0.5f;
            const float z = 1.f - std::fabs(px) - std::fabs(py);
            float n[3] = { px, -py, std::max(z, 0.f) };
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int i = 0; i < 3; ++i) texel[t][i] = len > 0 ? n[i] / len : (i == 2 ? 1.f : 0.f);
          } else {
            texel[t][0] = r[t] / 255.f;
          }
        }
      }
      for (int t = 0; t < 16; ++t) {
        const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
        if (x < img.w && y < img.h) std::memcpy(img.at(x, y), texel[t], img.c * sizeof(float));
      }
    }
  }
  return img;
}

void gaussianBlur(Img& img, float radius, bool wrap) {
  const float sigma = std::max(0.3f, radius / 2.f);
  const int r = std::max(1, int(std::ceil(sigma * 3.f)));
  std::vector<float> k(2 * r + 1);
  float sum = 0.f;
  for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5f * i * i / (sigma * sigma));
  for (float& v : k) v /= sum;
  std::vector<float> tmp(img.px.size());
  auto idx = [&](int v, int n) { return wrap ? ((v % n) + n) % n : std::clamp(v, 0, n - 1); };
  for (uint32_t y = 0; y < img.h; ++y)
    for (uint32_t x = 0; x < img.w; ++x)
      for (uint32_t c = 0; c < img.c; ++c) {
        float acc = 0.f;
        for (int i = -r; i <= r; ++i) acc += k[i + r] * img.px[(size_t(y) * img.w + idx(int(x) + i, img.w)) * img.c + c];
        tmp[(size_t(y) * img.w + x) * img.c + c] = acc;
      }
  for (uint32_t y = 0; y < img.h; ++y)
    for (uint32_t x = 0; x < img.w; ++x)
      for (uint32_t c = 0; c < img.c; ++c) {
        float acc = 0.f;
        for (int i = -r; i <= r; ++i) acc += k[i + r] * tmp[(size_t(idx(int(y) + i, img.h)) * img.w + x) * img.c + c];
        img.px[(size_t(y) * img.w + x) * img.c + c] = acc;
      }
}

Img nextMip(const Img& s, bool maxFilter) {
  Img d;
  d.w = std::max(1u, s.w / 2);
  d.h = std::max(1u, s.h / 2);
  d.c = s.c;
  d.px.resize(size_t(d.w) * d.h * d.c);
  for (uint32_t y = 0; y < d.h; ++y)
    for (uint32_t x = 0; x < d.w; ++x)
      for (uint32_t c = 0; c < d.c; ++c) {
        float acc = 0.f, peak = 0.f;
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t sx = std::min(s.w - 1, x * 2 + (k & 1)), sy = std::min(s.h - 1, y * 2 + (k >> 1));
          const float v = s.px[(size_t(sy) * s.w + sx) * s.c + c];
          acc += v;
          peak = std::max(peak, v);
        }
        d.px[(size_t(y) * d.w + x) * d.c + c] = maxFilter ? peak : acc * 0.25f;
      }
  return d;
}

uint8_t u8(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.f), 0L, 255L)); }

void encodeLevel(Img img, int map, uint32_t format, std::vector<uint8_t>& out) {
  const uint32_t bw = blocksOf(img.w), bh = blocksOf(img.h);
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      float texel[16][4];
      for (int t = 0; t < 16; ++t) {
        const uint32_t x = std::min(img.w - 1, bx * 4 + (t & 3)), y = std::min(img.h - 1, by * 4 + (t >> 2));
        std::memcpy(texel[t], img.at(x, y), img.c * sizeof(float));
      }
      uint8_t block[16];
      if (format == kFormatBC1 || format == kFormatBC3) {
        uint8_t rgb[16][3], a[16];
        for (int t = 0; t < 16; ++t) {
          for (int i = 0; i < 3; ++i) rgb[t][i] = u8(texel[t][i]);
          a[t] = u8(texel[t][3]);
        }
        if (format == kFormatBC3) {
          encodeBC4(a, block);
          encodeColor(rgb, block + 8);
          out.insert(out.end(), block, block + 16);
        } else {
          encodeColor(rgb, block);
          out.insert(out.end(), block, block + 8);
        }
      } else {
        uint8_t r[16], g[16];
        for (int t = 0; t < 16; ++t) {
          if (map == kNormal) {
            float n[3] = { texel[t][0], texel[t][1], texel[t][2] };
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (float& v : n) v = len > 0 ? v / len : 0.f;
            const float x = n[0], y = -n[1], z = std::max(n[2], 0.f);
            const float s = std::fabs(x) + std::fabs(y) + z;
            const float px = s > 0 ? x / s : 0.f, py = s > 0 ? y / s : 0.f;
            r[t] = u8((px + py) * 0.5f + 0.5f);
            g[t] = u8((px - py) * 0.5f + 0.5f);
          } else {
            r[t] = u8(texel[t][0]);
            g[t] = 0;
          }
        }
        encodeBC4(r, block);
        encodeBC4(g, block + 8);
        out.insert(out.end(), block, block + 16);
      }
    }
  }
}

bool hasAdjust(int map, const MapEdit& me, const Edit& e) {
  const bool extra = (map == kAlbedo && (e.albedoSaturation != 1.f || e.tint[0] != 1.f || e.tint[1] != 1.f ||
                                         e.tint[2] != 1.f)) ||
                     (map == kNormal && e.normalStrength != 1.f);
  return extra || !me.identity();
}

// The edit's adjustments on a decoded map (albedo RGBA, normal unit xyz, grey maps one channel).
void adjust(Img& img, int map, const MapEdit& me, const Edit& e) {
  const size_t n = size_t(img.w) * img.h;
  if (map == kAlbedo) {
    float mean[3] = {};
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c) mean[c] += img.px[i * 4 + c];
    const float lumMean = (mean[0] * 0.2126f + mean[1] * 0.7152f + mean[2] * 0.0722f) / float(n);
    for (size_t i = 0; i < n; ++i) {
      float* v = &img.px[i * 4];
      for (int c = 0; c < 3; ++c) {
        v[c] = me.invert ? 1.f - v[c] : v[c];
        v[c] = (v[c] - lumMean) * me.contrast + lumMean;
        v[c] *= 1.f + me.brightness;
      }
      const float l = v[0] * 0.2126f + v[1] * 0.7152f + v[2] * 0.0722f;
      for (int c = 0; c < 3; ++c) v[c] = std::clamp((l + (v[c] - l) * e.albedoSaturation) * e.tint[c], 0.f, 1.f);
    }
  } else if (map == kNormal) {
    for (size_t i = 0; i < n; ++i) {
      float* v = &img.px[i * 3];
      const float s = me.invert ? -e.normalStrength : e.normalStrength;
      v[0] *= s;
      v[1] *= s;
    }
  } else {
    float mean = 0.f;
    for (size_t i = 0; i < n; ++i) mean += img.px[i];
    mean /= float(n);
    for (size_t i = 0; i < n; ++i) {
      float v = me.invert ? 1.f - img.px[i] : img.px[i];
      const float m = me.invert ? 1.f - mean : mean;
      img.px[i] = std::clamp((v - m) * me.contrast + m + me.brightness, 0.f, 1.f);
    }
  }
  if (me.blur > 0.f) {
    gaussianBlur(img, me.blur, true);  // the maps tile (tools/pbr workflow pads them seamlessly)
  }
}

void process(Packed& p, int map, const MapEdit& me, const Edit& e) {
  if (!hasAdjust(map, me, e)) {
    return;
  }
  Img img = decodeLevel0(p, map);
  adjust(img, map, me, e);
  std::vector<uint8_t> data;
  uint32_t levels = 0;
  for (;;) {
    encodeLevel(img, map, p.format, data);
    ++levels;
    if (std::min(img.w, img.h) <= 4) {
      break;
    }
    img = nextMip(img, map == kHeight);
  }
  p.data = std::move(data);
  p.levels = levels;
}


// ---- permanent inversion (material editor: "Inverti ... (permanente)")

std::wstring mapsDir(uint64_t hash) {
  return folder() + L"\\maps\\" + hashName(hash) + L"\\" + variant();
}

std::wstring bakePath(uint64_t hash) {
  return mapsDir(hash) + L"\\" + hashName(hash) + L"_bake.txt";
}

// Newest <hash>_<map>_NNNNN.png of the folder (the workflow's newest run), empty when none.
std::wstring newestPng(uint64_t hash, int map) {
  const std::wstring dir = mapsDir(hash);
  const std::wstring prefix = hashName(hash) + L"_" + std::wstring(kKeys[map], kKeys[map] + std::strlen(kKeys[map])) + L"_";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\" + prefix + L"*.png").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::wstring best;
  long bestIndex = -1;
  do {
    const std::wstring name = fd.cFileName;
    const long index = std::wcstol(name.c_str() + prefix.size(), nullptr, 10);
    if (index > bestIndex) {
      bestIndex = index;
      best = dir + L"\\" + name;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return best;
}

// PNG <-> 32-bit ARGB pixels (0xAARRGGBB) through GDI+. The workflow's PNGs are 8-bit per channel.
const CLSID kPngEncoder = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };

struct GdiplusSession {
  ULONG_PTR token = 0;
  bool ok = false;
  GdiplusSession() {
    Gdiplus::GdiplusStartupInput input;
    ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }
  ~GdiplusSession() {
    if (ok) Gdiplus::GdiplusShutdown(token);
  }
};

bool readPng(const std::wstring& path, std::vector<uint32_t>& px, UINT& w, UINT& h) {
  Gdiplus::Bitmap src(path.c_str());  // keeps the file open until destroyed: read, then release before saving
  w = src.GetWidth();
  h = src.GetHeight();
  Gdiplus::Rect rect(0, 0, INT(w), INT(h));
  Gdiplus::BitmapData data;
  if (src.GetLastStatus() != Gdiplus::Ok || !w || !h ||
      src.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
    return false;
  }
  px.resize(size_t(w) * h);
  for (UINT y = 0; y < h; ++y) {
    std::memcpy(&px[size_t(y) * w], static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride, w * 4);
  }
  src.UnlockBits(&data);
  return true;
}

bool writePng(const std::wstring& path, std::vector<uint32_t>& px, UINT w, UINT h) {
  Gdiplus::Bitmap out(INT(w), INT(h), INT(w * 4), PixelFormat32bppARGB, reinterpret_cast<BYTE*>(px.data()));
  return out.Save(path.c_str(), &kPngEncoder, nullptr) == Gdiplus::Ok;
}

// Inverts the PNG in place: height -> every channel 1 - v; normal -> X and Y negated (red and green 1 - v, OpenGL
// encoding of the workflow), blue kept.
bool invertPng(const std::wstring& path, int map) {
  GdiplusSession gdi;
  UINT w = 0, h = 0;
  std::vector<uint32_t> px;
  if (!gdi.ok || !readPng(path, px, w, h)) {
    return false;
  }
  const uint32_t mask = map == kNormal ? 0x00FFFF00u : 0x00FFFFFFu;
  for (uint32_t& v : px) v ^= mask;
  return writePng(path, px, w, h);
}

// Applies the edit's adjustments of `map` to its PNG in place (same math as on the packed map).
bool adjustPng(const std::wstring& path, int map, const MapEdit& me, const Edit& e) {
  GdiplusSession gdi;
  UINT w = 0, h = 0;
  std::vector<uint32_t> px;
  if (!gdi.ok || !readPng(path, px, w, h)) {
    return false;
  }
  Img img;
  img.w = w;
  img.h = h;
  img.c = map == kAlbedo ? 4 : (map == kNormal ? 3 : 1);
  img.px.resize(size_t(w) * h * img.c);
  for (size_t i = 0; i < px.size(); ++i) {
    const float r = (px[i] >> 16 & 0xFF) / 255.f, g = (px[i] >> 8 & 0xFF) / 255.f, b = (px[i] & 0xFF) / 255.f;
    float* v = &img.px[i * img.c];
    if (map == kAlbedo) {
      v[0] = r, v[1] = g, v[2] = b, v[3] = (px[i] >> 24) / 255.f;
    } else if (map == kNormal) {
      float n[3] = { r * 2.f - 1.f, g * 2.f - 1.f, b * 2.f - 1.f };
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (int k = 0; k < 3; ++k) v[k] = len > 0 ? n[k] / len : (k == 2 ? 1.f : 0.f);
    } else {
      v[0] = r;
    }
  }
  adjust(img, map, me, e);
  for (size_t i = 0; i < px.size(); ++i) {
    const float* v = &img.px[i * img.c];
    uint32_t r, g, b, a = 255;
    if (map == kAlbedo) {
      r = u8(v[0]), g = u8(v[1]), b = u8(v[2]), a = u8(v[3]);
    } else if (map == kNormal) {
      float n[3] = { v[0], v[1], std::max(v[2], 0.f) };
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (float& c : n) c = len > 0 ? c / len : 0.f;
      r = u8(n[0] * 0.5f + 0.5f), g = u8(n[1] * 0.5f + 0.5f), b = u8(n[2] * 0.5f + 0.5f);
    } else {
      r = g = b = u8(v[0]);
    }
    px[i] = a << 24 | r << 16 | g << 8 | b;
  }
  return writePng(path, px, w, h);
}

bool writePacked(const std::wstring& path, const Packed& p) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) {
    return false;
  }
  const uint32_t header[4] = { p.format, p.width, p.height, p.levels };
  bool ok = fwrite("AC1T", 1, 4, f) == 4 && fwrite(header, 4, 4, f) == 4 &&
            fwrite(p.data.data(), 1, p.data.size(), f) == p.data.size();
  fclose(f);
  return ok;
}

std::wstring savePath(uint64_t hash) {
  return mapsDir(hash) + L"\\" + hashName(hash) + L"_save.json";
}

std::wstring packedPath(uint64_t hash, int map) {
  return mapsDir(hash) + L"\\" + hashName(hash) + L"_" + std::wstring(kKeys[map], kKeys[map] + std::strlen(kKeys[map])) +
         L".ac1t";
}

} // namespace

Edit readEditFile(const std::wstring& path);

bool bakePending(uint64_t hash) {
  return GetFileAttributesW(bakePath(hash).c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW(savePath(hash).c_str()) != INVALID_FILE_ATTRIBUTES;
}

uint32_t bakeRequests(uint64_t hash) {
  uint32_t baked = 0;
  // "Salva nelle texture": <hash>_save.json holds the adjustments to write into the map files
  const std::wstring save = savePath(hash);
  if (GetFileAttributesW(save.c_str()) != INVALID_FILE_ATTRIBUTES) {
    const Edit e = readEditFile(save);
    DeleteFileW(save.c_str());
    for (int m = 0; m < kMapCount; ++m) {
      if (!e.map[m].enabled || !hasAdjust(m, e.map[m], e)) {
        continue;
      }
      Packed packed;
      const bool packedOk = loadPackedFile(packedPath(hash, m), packed) &&
                            (process(packed, m, e.map[m], e), writePacked(packedPath(hash, m), packed));
      const std::wstring png = newestPng(hash, m);
      const bool pngOk = !png.empty() && adjustPng(png, m, e.map[m], e);
      log::line("pbr: %s of %016llX saved into the textures (.ac1t %s, png %s)", kKeys[m],
                static_cast<unsigned long long>(hash), packedOk ? "ok" : "FAILED", pngOk ? "ok" : "FAILED");
      if (packedOk) {
        baked |= 1u << m;
      }
    }
  }
  const std::wstring request = bakePath(hash);
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return baked;
  }
  std::string text;
  if (FILE* f = _wfopen(request.c_str(), L"rb")) {
    char buf[1024];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    fclose(f);
  }
  DeleteFileW(request.c_str());
  // each line "<map> invert"; two inversions of the same map cancel out
  int count[kMapCount] = {};
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = std::min(text.find('\n', pos), text.size());
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    for (int m : { int(kNormal), int(kHeight) }) {
      if (line.rfind(std::string(kKeys[m]) + " invert", 0) == 0) {
        ++count[m];
      }
    }
  }
  for (int m : { int(kNormal), int(kHeight) }) {
    if (count[m] % 2 == 0) {
      continue;
    }
    const std::wstring packedPath = mapsDir(hash) + L"\\" + hashName(hash) + L"_" +
                                    std::wstring(kKeys[m], kKeys[m] + std::strlen(kKeys[m])) + L".ac1t";
    Override maps;
    if (!loadPackedFile(packedPath, maps.packed[m])) {
      log::line("pbr: invert %s of %016llX: no packed map", kKeys[m], static_cast<unsigned long long>(hash));
      continue;
    }
    MapEdit invert;
    invert.invert = true;
    process(maps.packed[m], m, invert, Edit {});
    const bool packedOk = writePacked(packedPath, maps.packed[m]);
    const std::wstring png = newestPng(hash, m);
    const bool pngOk = !png.empty() && invertPng(png, m);
    log::line("pbr: %s of %016llX inverted permanently (.ac1t %s, png %s)", kKeys[m],
              static_cast<unsigned long long>(hash), packedOk ? "ok" : "FAILED", pngOk ? "ok" : "FAILED");
    if (packedOk) {
      baked |= 1u << m;
    }
  }
  return baked;
}

std::wstring editPath(uint64_t hash) {
  const std::wstring name = hashName(hash);
  return folder() + L"\\maps\\" + name + L"\\" + variant() + L"\\" + name + L"_edit.json";
}

uint64_t editStamp(uint64_t hash) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(editPath(hash).c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

Edit readEditFile(const std::wstring& path) {
  Edit e;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) {
    return e;
  }
  std::string j;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) j.append(buf, got);
  fclose(f);
  e.present = true;
  float v;
  if (number(j, "enabled", v)) e.enabled = v != 0.f;
  for (int m = 0; m < kMapCount; ++m) {
    const std::string k = kKeys[m];
    if (number(j, k + "_enabled", v)) e.map[m].enabled = v != 0.f;
    number(j, k + "_brightness", e.map[m].brightness);
    number(j, k + "_contrast", e.map[m].contrast);
    number(j, k + "_blur", e.map[m].blur);
    if (number(j, k + "_invert", v)) e.map[m].invert = v != 0.f;
  }
  number(j, "albedo_saturation", e.albedoSaturation);
  number(j, "tint_r", e.tint[0]);
  number(j, "tint_g", e.tint[1]);
  number(j, "tint_b", e.tint[2]);
  number(j, "normal_strength", e.normalStrength);
  if (number(j, "pom_enabled", v)) e.pomEnabled = v != 0.f;
  number(j, "pom_depth_cm", e.pomDepthCm);
  number(j, "pom_out_cm", e.pomOutCm);
  number(j, "pom_in_cm", e.pomInCm);
  return e;
}

Edit readEdit(uint64_t hash) {
  return readEditFile(editPath(hash));
}

void applyEdit(Override& maps, const Edit& edit) {
  if (!edit.present || !maps.isPacked) {
    return;
  }
  for (int m = 0; m < kMapCount; ++m) {
    Packed& p = maps.packed[m];
    if (p.empty()) {
      continue;
    }
    if (!edit.map[m].enabled && m != kAlbedo) {
      p = {};  // removed from the material (the albedo stays: it carries the material's identity for picking)
      continue;
    }
    process(p, m, edit.map[m], edit);
  }
}

} // namespace ac1rtx::pbr
