#include "anticull.h"

#include "hook.h"
#include "log.h"
#include "skinned.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>

// Every address, offset and constant below was read from AssassinsCreed_Dx9.exe v1.02 (decompilation/disassembly);
// the evidence is in docs/notes/culling_lod.md and kb.h ("Visibility / LOD").

namespace ac1rtx::anticull {

namespace {

// ---- Per-view visibility (FUN_009E6600 / FUN_009E5390, once per view per frame) ----
// X = [view + 0x1E0]; FUN_00A2FFD0(X, view) fills the query Y = X + 0x750 (FUN_00A2FCC0), then
// FUN_00AA0940(X) -> FUN_00AC6B90(container, Y, 0). The container holds every registered object/light of the
// graphic world in flat lists; each entry is tested and, if accepted, pushed into X's visible lists.
constexpr uintptr_t kContainerVisibility = 0x00AC6B90;  // __thiscall(container, Y, arg), ret 8
constexpr uint8_t kContainerVisibilityPrologue[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57 };
constexpr uintptr_t kQueryInView = 0x750;               // Y = X + 0x750 (0xA2FCCD, 0xAA0946)
constexpr uintptr_t kContainerObjects = 0x5C;           // {entry*, count}: render objects, tested by FUN_00AC5CE0
constexpr uintptr_t kContainerLights = 0x68;            // {entry*, count}: light objects, tested by FUN_00AC5860

// X's output lists {ptr, count, capacity | 0x80000000 owned}; the push callbacks 0xA2F770 / 0xA2F7A0 do not check
// the capacity. Reserve functions (thiscall(list, n), ret 4) free the old buffer when bit 31 is set and reset count.
constexpr uintptr_t kVisibleObjects = 0xA40;            // capacity 0x2000 (FUN_009E0C00 -> FUN_00A30050)
constexpr uintptr_t kVisibleLights = 0xA4C;             // capacity 0x80
constexpr uintptr_t kAlwaysLists = 0xA58;               // 8 x {entry*, count & 0x3FFF}, 12-byte entries (FUN_00A9F870)
constexpr uint32_t kAlwaysListCount = 8;
constexpr uintptr_t kReserveVisibleObjects = 0x00A2F3F0;
constexpr uintptr_t kReserveVisibleLights = 0x00A2F490;
// Render item id (+8 of each 20-byte item) = visible index; the renderer's occlusion id bitset / query slots hold
// 0x4000 ids (FUN_00ABCC90: FUN_00ABC8D0(0x4000), bitset 0x800 bytes).
constexpr uint32_t kItemIdCapacity = 0x4000;

// Query distance limits read by FUN_00AC5CE0: an object flagged small (+0xA0 bit 20) / medium (bit 21) whose
// distance to the eye exceeds these is culled. FUN_00AC6E90 derives them from the view surface settings and the
// Assassin.ini Small/MediumObjectsCullDistanceModifier; FUN_00AC7120 resets them to FLT_MAX (no limit).
constexpr uintptr_t kQuerySmallCullDistance = 0x2D8;
constexpr uintptr_t kQueryMediumCullDistance = 0x2DC;

// ---- Frustum classification FUN_00AC5860 ----
// __thiscall(object, clip, channelMask, Y, visibleSet, visibleMask, shadowSet, shadowMask), ret 0x1C.
// clip bit 1: test the camera frustum (Y + 0 = 6 planes, FUN_009F4010). Outside -> object+0x20 &= visibleMask and
// the result gets bit 2 (0xAC58B1..0xAC58B7); the shadow-volume test then runs as usual. channelMask != 0 and no
// common byte with (object+0x24 >> 3) -> rejected before any frustum test.
constexpr uintptr_t kFrustumClassify = 0x00AC5860;
constexpr uint8_t kFrustumClassifyPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
constexpr uint32_t kClipTestFrustum = 1u << 1;
constexpr uint32_t kClipOutsideFrustum = 1u << 2;
// Return addresses of the two engine call sites: 0xAC5DFF in FUN_00AC5CE0 (container objects list) and 0xAC61CF in
// FUN_00AC6110 (container lights list).
constexpr uintptr_t kReturnFromObjectTest = 0x00AC5E04;
constexpr uintptr_t kReturnFromLightTest = 0x00AC61D4;

// Render object (the Submit `placement`): +0x20 pass bits, +0x24 low byte >> 3 light channels, +0xA0 bits 0-3
// component count (FUN_00A9F420 loops over +0xB0[i]).
constexpr uintptr_t kObjectPassBits = 0x20;
constexpr uintptr_t kObjectFlags = 0x24;
constexpr uintptr_t kObjectVisibility = 0xA0;
constexpr uint32_t kObjectComponentMask = 0xF;

// ---- Render sets gather FUN_00A9F910: __thiscall(sets, X), ret 4 ----
// sets = renderer + 0x1DC: 48 lists (8 groups x 6 passes) of 0x18 bytes {item*, count, capacity}; FUN_00A9F540
// pushes 20-byte items without a capacity check. FUN_00A9F740 allocates 20 of them (0x10..0x1000 items) through
// FUN_00A9F690 (thiscall(list, n), ret 4); the others stay unallocated and are never pushed to.
constexpr uintptr_t kRenderSetsGather = 0x00A9F910;
constexpr uint8_t kRenderSetsGatherPrologue[] = { 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57 };
constexpr uintptr_t kReserveRenderList = 0x00A9F690;
constexpr uint32_t kRenderListCount = 48;
constexpr uintptr_t kRenderListStride = 0x18;

// ---- LOD selection: LODSelectorInstance slot 21 (vtable 0x016C2344 + 0x54) = FUN_00A90160 ----
// __thiscall(instance, object, sets, id, group), ret 0x10. res = [instance + 0x10] (scimitar::LODSelector);
// lod = FUN_00A8F510(res + 0x60, fovScale * object+0xA8 distance): 256-entry 2-bit table built by FUN_00A90C30
// from the LOD descriptors' far distances; the slot instance = [instance + 0x14 + lod*4], handle at +8, pass mask
// byte at +0xE. The engine then may raise lod to a per-object class minimum and cross-fade to the next LOD
// (group 3 with stencil dithering). The engine's non-blended add is FUN_00A9F540(handle, object, id, passMask,
// group 0, fade 0xFF, stencil 0).
constexpr uintptr_t kLodSelectorAddToRenderSets = 0x00A90160;
constexpr uint8_t kLodSelectorAddToRenderSetsPrologue[] = { 0x83, 0xEC, 0x14, 0x8B, 0x44, 0x24, 0x18, 0x53, 0x55 };
constexpr uintptr_t kLodTableLookup = 0x00A8F510;       // __thiscall(table, float distance), ret 4 -> lod & 3
constexpr uintptr_t kAddToRenderLists = 0x00A9F540;     // __thiscall(sets, handle, object, id, passMask, group,
                                                        //            fade, stencil), ret 0x1C
constexpr uintptr_t kLodSelectorResource = 0x10;
constexpr uintptr_t kLodSelectorTable = 0x60;
constexpr uintptr_t kLodSelectorSlots = 0x14;
constexpr uintptr_t kLodSlotHandle = 0x08;
constexpr uintptr_t kLodSlotPassMask = 0x0E;
constexpr uint32_t kUnblendedGroup = 0;
constexpr uint32_t kOpaqueFade = 0xFF;
constexpr uint32_t kNoStencil = 0;

using ContainerVisibilityFn = void(__thiscall*)(void* container, uint8_t* query, uint32_t arg);
using FrustumClassifyFn = uint32_t(__thiscall*)(uint8_t* object, uint32_t clip, uint32_t channelMask, uint8_t* query,
                                                uint32_t visibleSet, uint32_t visibleMask, uint32_t shadowSet,
                                                uint32_t shadowMask);
using RenderSetsGatherFn = void(__thiscall*)(uint8_t* sets, uint8_t* x);
using LodAddFn = void(__thiscall*)(void* instance, void* object, void* sets, uint32_t id, uint32_t group);
using LodTableLookupFn = uint32_t(__thiscall*)(const void* table, float distance);
using AddToRenderListsFn = void(__thiscall*)(void* sets, uint32_t handle, void* object, uint32_t id,
                                             uint32_t passMask, uint32_t group, uint32_t fade, uint32_t stencil);
using ReserveFn = void(__thiscall*)(void* list, uint32_t capacity);

ContainerVisibilityFn s_originalContainerVisibility = nullptr;
FrustumClassifyFn s_originalFrustumClassify = nullptr;
RenderSetsGatherFn s_originalRenderSetsGather = nullptr;
LodAddFn s_originalLodAdd = nullptr;

const auto s_lodTableLookup = reinterpret_cast<LodTableLookupFn>(kLodTableLookup);
const auto s_addToRenderLists = reinterpret_cast<AddToRenderListsFn>(kAddToRenderLists);
const auto s_reserveVisibleObjects = reinterpret_cast<ReserveFn>(kReserveVisibleObjects);
const auto s_reserveVisibleLights = reinterpret_cast<ReserveFn>(kReserveVisibleLights);
const auto s_reserveRenderList = reinterpret_cast<ReserveFn>(kReserveRenderList);

// Runtime switches (F10 anticulling, F11 max LOD) so their cost can be compared in game; the hooks stay installed.
volatile bool s_cullActive = true;
volatile bool s_lodActive = true;
Settings s_settings;

// Objects kept off-screen by the frustum override, tagged with the view query generation that kept them: their LOD
// add uses the coarsest slot (reflections / GI / shadows only). Visibility and gather run per view, in that order.
std::mutex s_offscreenMutex;
std::unordered_map<const void*, uint32_t> s_offscreen;
std::atomic<uint32_t> s_viewGeneration { 0 };

// Render object size classes and distance (FUN_00AC5CE0): +0xA0 bit 20 small, bit 21 medium (neither = large);
// +0xA8 eye -> AABB distance, written at 0xAC5D5A before the frustum classification call at 0xAC5DFF.
constexpr uint32_t kObjectSmall = 1u << 20;
constexpr uint32_t kObjectMedium = 1u << 21;
constexpr uintptr_t kObjectDistance = 0xA8;
// LOD inputs (FUN_00A90160): distance = [[sets + 0x484] + 0x2AC] (fov scale) * object+0xA8; table = res + 0x60 whose
// first float is the quantisation scale (entry i covers distance i / scale, 256 entries; FUN_00A90C30); class
// minimum: object+0xA4 & 7 = class c, minimum slot = ([res + 0xA4] >> (2c - 2)) & 3.
constexpr uintptr_t kSetsView = 0x484;
constexpr uintptr_t kViewFovScale = 0x2AC;
constexpr uintptr_t kObjectLodClass = 0xA4;
constexpr uintptr_t kLodClassMinimums = 0xA4;  // in the selector resource (table + 0x44)

// The view query being evaluated on this thread and which of its lists may take objects the frustum rejected.
struct Scope {
  const uint8_t* query = nullptr;
  bool objects = false;
  bool lights = false;
};
thread_local Scope t_scope;

template<typename T>
T& at(void* base, uintptr_t offset) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

template<typename T>
T at(const void* base, uintptr_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}

uint32_t listCapacity(const void* list) {
  return at<uint32_t>(list, 8) & 0x7FFFFFFFu;
}

// Grows an engine list with its own reserve function so `needed` entries fit. Doubling bounds the number of
// reallocations (and the buffers leaked by lists the engine does not own) as the loaded world grows.
bool ensureCapacity(void* list, uint32_t needed, ReserveFn reserve, const char* name) {
  const uint32_t capacity = listCapacity(list);
  if (needed <= capacity) {
    return true;
  }
  if (at<uint32_t>(list, 4) != 0) {
    return false;  // the list already holds this frame's entries
  }
  const uint32_t grown = needed > capacity * 2 ? needed : capacity * 2;
  reserve(list, grown);
  const bool ok = at<void*>(list, 0) != nullptr && listCapacity(list) >= needed;
  log::line("anticull: %s list %p grown %u -> %u (%s)", name, list, capacity, grown, ok ? "ok" : "FAILED");
  return ok;
}

uint32_t alwaysDrawnCount(const void* x) {
  uint32_t total = 0;
  for (uint32_t k = 0; k < kAlwaysListCount; ++k) {
    total += at<uint32_t>(x, kAlwaysLists + k * 8 + 4) & 0x3FFFu;
  }
  return total;
}

void pollToggles() {
  static bool s_f10 = false, s_f11 = false;
  const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
  if (f10 && !s_f10) {
    s_cullActive = !s_cullActive;
    log::line("anticull: AntiCulling %s (F10)", s_cullActive ? "ON" : "OFF");
  }
  if (f11 && !s_f11) {
    s_lodActive = !s_lodActive;
    log::line("anticull: ForceMaxLod %s (F11)", s_lodActive ? "ON" : "OFF");
  }
  s_f10 = f10;
  s_f11 = f11;
}

void __fastcall containerVisibilityHook(void* container, void* /*edx*/, uint8_t* query, uint32_t arg) {
  uint8_t* x = query - kQueryInView;
  const uint32_t objects = at<uint32_t>(container, kContainerObjects + 4);
  const uint32_t lights = at<uint32_t>(container, kContainerLights + 4);

  Scope scope;
  scope.query = query;
  scope.objects = objects + alwaysDrawnCount(x) <= kItemIdCapacity &&
                  ensureCapacity(x + kVisibleObjects, objects, s_reserveVisibleObjects, "visible objects");
  scope.lights = ensureCapacity(x + kVisibleLights, lights, s_reserveVisibleLights, "visible lights");

  static bool s_loggedGate = false;
  if ((!scope.objects || !scope.lights) && !s_loggedGate) {
    s_loggedGate = true;
    log::line("anticull: view query %p kept engine culling (objects %u fit=%d, lights %u fit=%d)", query, objects,
              scope.objects ? 1 : 0, lights, scope.lights ? 1 : 0);
  }
  pollToggles();
  if (!s_cullActive) {
    scope.objects = scope.lights = false;
  }
  if (scope.objects) {
    // Small/medium on-screen culls: the engine's distances (FUN_00AC6E90) scaled, not removed (0 = no limit).
    float& small = at<float>(query, kQuerySmallCullDistance);
    float& medium = at<float>(query, kQueryMediumCullDistance);
    static bool s_loggedDistances = false;
    if (!s_loggedDistances) {
      s_loggedDistances = true;
      log::line("anticull: engine cull distances small %.1f medium %.1f -> x%.2f", small, medium,
                s_settings.cullDistanceScale);
    }
    const float scale = s_settings.cullDistanceScale;
    small = scale > 0.0f ? small * scale : std::numeric_limits<float>::max();
    medium = scale > 0.0f ? medium * scale : std::numeric_limits<float>::max();
  }
  ++s_viewGeneration;

  t_scope = scope;
  s_originalContainerVisibility(container, query, arg);
  t_scope = {};
}

uint32_t __fastcall frustumClassifyHook(uint8_t* object, void* /*edx*/, uint32_t clip, uint32_t channelMask,
                                        uint8_t* query, uint32_t visibleSet, uint32_t visibleMask, uint32_t shadowSet,
                                        uint32_t shadowMask) {
  const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
  uint32_t result = s_originalFrustumClassify(object, clip, channelMask, query, visibleSet, visibleMask, shadowSet,
                                              shadowMask);
  if (query != t_scope.query || !(clip & kClipTestFrustum) || !(result & kClipOutsideFrustum)) {
    return result;
  }
  const bool allowed = (caller == kReturnFromObjectTest && t_scope.objects) ||
                       (caller == kReturnFromLightTest && t_scope.lights);
  const auto channels = static_cast<uint8_t>(at<uint32_t>(object, kObjectFlags) >> 3);
  if (!allowed || (channelMask != 0 && (static_cast<uint8_t>(channelMask) & channels) == 0)) {
    return result;
  }
  if (caller == kReturnFromObjectTest) {
    // Off-screen objects only matter to the path tracer through reflections, GI and shadows: keep them by size class
    // within a radius (small ones never), and draw them at their coarsest LOD (lodAddHook).
    const uint32_t flags = at<uint32_t>(object, kObjectVisibility);
    const float distance = at<float>(object, kObjectDistance);
    const float radius = (flags & kObjectSmall)    ? s_settings.offscreenRadiusSmall
                         : (flags & kObjectMedium) ? s_settings.offscreenRadiusMedium
                                                   : s_settings.offscreenRadiusLarge;
    if (!(distance <= radius)) {
      return result;
    }
    std::lock_guard lock { s_offscreenMutex };
    s_offscreen[object] = s_viewGeneration.load();
  }
  // Same effect as the engine's in-frustum branch for the camera pass; the shadow bits keep the engine's verdict.
  at<uint32_t>(object, kObjectPassBits) |= visibleSet;
  return result | kClipTestFrustum;
}

void __fastcall renderSetsGatherHook(uint8_t* sets, void* /*edx*/, uint8_t* x) {
  // Upper bound of items any single list can receive: each visible object's components add at most one item per
  // list, each always-drawn entry one item.
  const auto* visible = at<void* const*>(x, kVisibleObjects);
  const uint32_t visibleCount = at<uint32_t>(x, kVisibleObjects + 4);
  uint32_t needed = alwaysDrawnCount(x);
  for (uint32_t i = 0; visible && i < visibleCount; ++i) {
    needed += at<uint32_t>(visible[i], kObjectVisibility) & kObjectComponentMask;
  }
  for (uint32_t list = 0; list < kRenderListCount; ++list) {
    uint8_t* entry = sets + list * kRenderListStride;
    if (listCapacity(entry) != 0) {  // lists the engine never allocated are never pushed to
      ensureCapacity(entry, needed, s_reserveRenderList, "render");
    }
  }
  s_originalRenderSetsGather(sets, x);
}

void __fastcall lodAddHook(void* instance, void* /*edx*/, void* object, void* sets, uint32_t id, uint32_t group) {
  // Characters keep the engine's LOD (forcing LOD0 distorted distant NPCs). `object` is stored at render item +0
  // (FUN_00A9F540 0xA9F5F5) and passed by RenderBatch_DrawItems as Submit's placement (0xABB3DE-0xABB3F2), the same
  // pointer the skinned palette hook records: a plain pointer comparison, no engine memory is read.
  if (!s_lodActive || skinned::isSkinnedPlacement(object)) {
    s_originalLodAdd(instance, object, sets, id, group);
    return;
  }
  const void* selector = at<const void*>(instance, kLodSelectorResource);
  const auto* table = static_cast<const uint8_t*>(selector) + kLodSelectorTable;
  const float eyeDistance = at<float>(object, kObjectDistance);
  bool offscreen = false;
  {
    std::lock_guard lock { s_offscreenMutex };
    auto it = s_offscreen.find(object);
    offscreen = it != s_offscreen.end() && it->second == s_viewGeneration.load();
  }
  uint32_t lod;
  if (offscreen) {
    // Coarsest slot the selector has: the table entry of its last quantised distance (entry 255).
    const float tableScale = at<float>(table, 0);
    lod = s_lodTableLookup(table, tableScale > 0.0f ? 254.5f / tableScale : 0.0f) & 3u;
  } else if (eyeDistance <= s_settings.maxLodRadius) {
    lod = s_lodTableLookup(table, 0.0f) & 3u;  // most detailed slot
  } else {
    // Engine choice with its LOD distances pushed out by 1 / LodDistanceScale, plus its class minimum.
    const float fovScale = at<float>(at<const void*>(sets, kSetsView), kViewFovScale);
    lod = s_lodTableLookup(table, fovScale * eyeDistance * s_settings.lodDistanceScale) & 3u;
    const uint32_t cls = at<uint32_t>(object, kObjectLodClass) & 7u;
    if (cls) {
      const uint32_t minimum = (at<uint32_t>(selector, kLodClassMinimums) >> (2 * cls - 2)) & 3u;
      lod = minimum > lod ? minimum : lod;
    }
  }
  void* slot = at<void*>(instance, kLodSelectorSlots + lod * 4);
  (void)group;  // the engine's LODSelector add ignores it as well (always group 0, or 3 for the cross-fade)
  if (!slot) {
    return;
  }
  const uint32_t handle = at<uint32_t>(slot, kLodSlotHandle);
  if (handle == 0) {
    return;
  }
  s_addToRenderLists(sets, handle, object, id, at<uint8_t>(slot, kLodSlotPassMask), kUnblendedGroup, kOpaqueFade,
                     kNoStencil);
}

} // namespace

bool install(const Settings& settings) {
  s_settings = settings;
  bool ok = true;
  if (settings.antiCulling) {
    // Order matters: the frustum override is only installed once the list-capacity guards are in place, because
    // the engine's list pushes are unchecked.
    ok = hook::install("RenderSets_Gather", kRenderSetsGather, kRenderSetsGatherPrologue,
                       reinterpret_cast<void*>(&renderSetsGatherHook), &s_originalRenderSetsGather) &&
         hook::install("Visibility_Container", kContainerVisibility, kContainerVisibilityPrologue,
                       reinterpret_cast<void*>(&containerVisibilityHook), &s_originalContainerVisibility) &&
         hook::install("Visibility_FrustumClassify", kFrustumClassify, kFrustumClassifyPrologue,
                       reinterpret_cast<void*>(&frustumClassifyHook), &s_originalFrustumClassify);
  }
  if (settings.forceMaxLod) {
    ok = hook::install("LODSelectorInstance_AddToRenderSets", kLodSelectorAddToRenderSets,
                       kLodSelectorAddToRenderSetsPrologue, reinterpret_cast<void*>(&lodAddHook),
                       &s_originalLodAdd) && ok;
  }
  log::line("anticull: AntiCulling=%d ForceMaxLod=%d installed=%d", settings.antiCulling ? 1 : 0,
            settings.forceMaxLod ? 1 : 0, ok ? 1 : 0);
  return ok;
}

} // namespace ac1rtx::anticull
