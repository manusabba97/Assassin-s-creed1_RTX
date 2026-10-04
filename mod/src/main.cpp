// AC1RTX.asi - loaded by Ultimate ASI Loader (dinput8.dll) from <game>\scripts\.
// Reads engine data and submits it to RTX Remix through the Remix API exposed by the bridge client d3d9.dll.

#include "anticull.h"
#include "camera.h"
#include "dynamic_mesh.h"
#include "hud.h"
#include "lights.h"
#include "log.h"
#include "materials.h"
#include "pbr.h"
#include "remix.h"
#include "screenshot.h"
#include "skinned.h"
#include "static_geometry.h"

#include <windows.h>

#include <MinHook.h>

#include <cwchar>
#include <string>

namespace {

std::wstring siblingPath(HMODULE module, const wchar_t* fileName) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring result = path;
  result.resize(result.find_last_of(L"\\/") + 1);
  return result + fileName;
}

// The Remix runtime resolves rtx.conf against the current directory, which the ASI loader leaves at scripts\.
// The bridge server inherits this process' environment, so point it at <game dir>\rtx.conf explicitly.
void exportRtxConfigPath() {
  if (GetEnvironmentVariableW(L"DXVK_RTX_CONFIG_FILE", nullptr, 0) != 0) {
    return;
  }
  const std::wstring path = siblingPath(GetModuleHandleW(nullptr), L"rtx.conf");
  SetEnvironmentVariableW(L"DXVK_RTX_CONFIG_FILE", path.c_str());
  ac1rtx::log::line("DXVK_RTX_CONFIG_FILE=%ls", path.c_str());
}

// DXVK reads its HUD only from DXVK_HUD (dxvk_hud_item.cpp); the bridge server inherits this process' environment, so
// the counter shows however the game is started. [Debug] FpsHud, empty disables; an existing DXVK_HUD wins.
void exportHud(const std::wstring& iniPath) {
  if (GetEnvironmentVariableW(L"DXVK_HUD", nullptr, 0) != 0) {
    return;
  }
  wchar_t hud[128] = {};
  GetPrivateProfileStringW(L"Debug", L"FpsHud", L"fps,frametimes,scale=3", hud, 128, iniPath.c_str());
  if (hud[0]) {
    SetEnvironmentVariableW(L"DXVK_HUD", hud);
    ac1rtx::log::line("DXVK_HUD=%ls", hud);
  }
}

ac1rtx::remix::Settings readSettings(const std::wstring& iniPath) {
  ac1rtx::remix::Settings s;
  s.staticMeshes = GetPrivateProfileIntW(L"Geometry", L"StaticMeshes", 1, iniPath.c_str()) != 0;
  s.lights = GetPrivateProfileIntW(L"Lights", L"Enable", 1, iniPath.c_str()) != 0;
  wchar_t value[64] = {};
  GetPrivateProfileStringW(L"Lights", L"SphereRadius", L"0.1", value, 64, iniPath.c_str());
  s.lightSphereRadius = std::wcstof(value, nullptr);
  GetPrivateProfileStringW(L"Lights", L"RadianceScale", L"1.0", value, 64, iniPath.c_str());
  s.lightRadianceScale = std::wcstof(value, nullptr);
  return s;
}

void start(HMODULE module) {
  ac1rtx::log::open(siblingPath(module, L"AC1RTX.log").c_str());
  ac1rtx::log::line("AC1RTX loaded");
  exportRtxConfigPath();

  const std::wstring iniPath = siblingPath(module, L"AC1RTX.ini");
  exportHud(iniPath);
  const auto settings = readSettings(iniPath);

  const int screenshotKey = GetPrivateProfileIntW(L"Debug", L"ScreenshotKey", 0x50, iniPath.c_str());
  if (screenshotKey != 0) {
    wchar_t dir[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Debug", L"ScreenshotDir", L"", dir, MAX_PATH, iniPath.c_str());
    ac1rtx::screenshot::start(screenshotKey, dir[0] ? std::wstring(dir) : siblingPath(module, L"screens"));
  }
  ac1rtx::log::line("settings: StaticMeshes=%d Lights=%d SphereRadius=%.3f RadianceScale=%.3f",
                    settings.staticMeshes ? 1 : 0, settings.lights ? 1 : 0,
                    settings.lightSphereRadius, settings.lightRadianceScale);
  ac1rtx::remix::configure(settings);

  ac1rtx::pbr::Settings pbrSettings;
  pbrSettings.enable = GetPrivateProfileIntW(L"PBR", L"Enable", 0, iniPath.c_str()) != 0;
  // Dump (all PBR-eligible albedos since 2026-10-03); DumpTerrain is the older name of the key
  pbrSettings.dump = GetPrivateProfileIntW(L"PBR", L"Dump", GetPrivateProfileIntW(L"PBR", L"DumpTerrain", 0, iniPath.c_str()), iniPath.c_str()) != 0;
  pbrSettings.layerPom = GetPrivateProfileIntW(L"PBR", L"LayerPom", 0, iniPath.c_str()) != 0;
  pbrSettings.exportVegetation = GetPrivateProfileIntW(L"PBR", L"ExportVegetation", 0, iniPath.c_str()) != 0;
  wchar_t pbrFolder[MAX_PATH] = {};
  GetPrivateProfileStringW(L"PBR", L"Folder", L"", pbrFolder, MAX_PATH, iniPath.c_str());
  // a relative Folder is next to the game executable (release ini: AC1RTX_pbr), made absolute for the runtime's
  // editor, which runs in another process with another working directory
  if (pbrFolder[0] && !(pbrFolder[1] == L':' || (pbrFolder[0] == L'\\' && pbrFolder[1] == L'\\'))) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring full(exe);
    full = full.substr(0, full.find_last_of(L"\\/") + 1) + pbrFolder;
    wcsncpy_s(pbrFolder, full.c_str(), _TRUNCATE);
  }
  pbrSettings.folder = pbrFolder;
  wchar_t pbrVariant[16] = {};
  GetPrivateProfileStringW(L"PBR", L"Variant", L"x4", pbrVariant, 16, iniPath.c_str());
  pbrSettings.variant = pbrVariant;
  ac1rtx::pbr::configure(pbrSettings);
  // The Remix runtime's material editor (key M) reads and writes the maps folder; the bridge server inherits this
  // process' environment.
  if (pbrSettings.enable && pbrFolder[0]) {
    SetEnvironmentVariableW(L"AC1RTX_PBR_DIR", pbrFolder);
    SetEnvironmentVariableW(L"AC1RTX_PBR_VARIANT", pbrVariant);
  }

  if (MH_Initialize() != MH_OK) {
    ac1rtx::log::line("MH_Initialize failed");
    return;
  }
  ac1rtx::camera::install(&ac1rtx::remix::ensureInitialized, &ac1rtx::remix::onViewSetup);
  if (settings.staticMeshes) {
    ac1rtx::static_geometry::install();
    wchar_t dumpDir[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Debug", L"ShaderDumpDir", L"", dumpDir, MAX_PATH, iniPath.c_str());
    ac1rtx::materials::setShaderDumpDir(dumpDir);
    ac1rtx::materials::install();
    ac1rtx::skinned::install();
    ac1rtx::dynamic_mesh::install();
  }
  if (settings.lights) {
    ac1rtx::lights::install();
  }
  if (GetPrivateProfileIntW(L"HUD", L"Enable", 1, iniPath.c_str()) != 0) {
    ac1rtx::hud::install(&ac1rtx::remix::beginOverlay, &ac1rtx::remix::endOverlay, &ac1rtx::remix::beginPostProcess,
                         &ac1rtx::remix::beginRenderTargetDraw);
  }
  ac1rtx::anticull::Settings visibility;
  visibility.antiCulling = GetPrivateProfileIntW(L"Visibility", L"AntiCulling", 0, iniPath.c_str()) != 0;
  visibility.forceMaxLod = GetPrivateProfileIntW(L"Visibility", L"ForceMaxLod", 0, iniPath.c_str()) != 0;
  auto readFloat = [&](const wchar_t* key, float fallback) {
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"Visibility", key, L"", text, 64, iniPath.c_str());
    return text[0] ? std::wcstof(text, nullptr) : fallback;
  };
  visibility.offscreenRadiusLarge = readFloat(L"OffscreenRadiusLarge", visibility.offscreenRadiusLarge);
  visibility.offscreenRadiusMedium = readFloat(L"OffscreenRadiusMedium", visibility.offscreenRadiusMedium);
  visibility.offscreenRadiusSmall = readFloat(L"OffscreenRadiusSmall", visibility.offscreenRadiusSmall);
  visibility.cullDistanceScale = readFloat(L"CullDistanceScale", visibility.cullDistanceScale);
  visibility.maxLodRadius = readFloat(L"MaxLodRadius", visibility.maxLodRadius);
  visibility.lodDistanceScale = readFloat(L"LodDistanceScale", visibility.lodDistanceScale);
  ac1rtx::log::line("visibility: offscreen L/M/S %.0f/%.0f/%.0f m, cull x%.2f, LOD0 within %.0f m, LOD distance x%.2f",
                    visibility.offscreenRadiusLarge, visibility.offscreenRadiusMedium, visibility.offscreenRadiusSmall,
                    visibility.cullDistanceScale, visibility.maxLodRadius, visibility.lodDistanceScale);
  if (visibility.antiCulling || visibility.forceMaxLod) {
    ac1rtx::anticull::install(visibility);
  }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    start(module);
  }
  return TRUE;
}
