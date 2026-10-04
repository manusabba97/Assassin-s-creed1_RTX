#include "lights.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <mutex>
#include <unordered_set>

namespace ac1rtx::lights {

namespace {

using LightUpdateFn = void(__stdcall*)(void* x);
LightUpdateFn s_originalUpdate = nullptr;

std::mutex s_mutex;
Frame s_frame;
std::unordered_set<uintptr_t> s_unknownClasses;  // logged once

bool classify(uintptr_t vtable, Kind& kind) {
  switch (vtable) {
    case game::kOmniLightVtable: kind = Kind::Omni; return true;
    case game::kSpotLightVtable: kind = Kind::Spot; return true;
    case game::kDirectionalLightVtable: kind = Kind::Directional; return true;
    case game::kSunLightVtable: kind = Kind::Sun; return true;
    default: return false;  // DirectionalSphereLight is ignored by the engine's selector FUN_00AD5570 as well
  }
}

void copy3(const void* object, uintptr_t offset, float out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = game::field<float>(object, offset + i * 4);
  }
}

bool readLight(const void* object, Light& out) {
  const uint32_t flags = game::field<uint32_t>(object, game::kObjectFlags);
  const void* resource = game::field<const void*>(object, game::kLightResource);
  if ((flags & game::kObjectTypeMask) != game::kObjectTypeLight || !resource) {
    return false;
  }
  const uintptr_t vtable = game::field<uintptr_t>(resource, 0);
  if (!classify(vtable, out.kind)) {
    if (s_unknownClasses.insert(vtable).second) {
      log::line("light %p: resource %p vtable %08X not a light class the engine applies - skipped", object, resource,
                unsigned(vtable));
    }
    return false;
  }
  out.object = object;
  out.resource = resource;
  out.channels = (flags >> game::kObjectChannelShift) & game::kObjectChannelMask;
  copy3(object, game::kLightPosition, out.position);
  copy3(object, game::kLightAxisY, out.axisY);
  const float intensity = game::field<float>(resource, game::kLightIntensity);
  copy3(resource, game::kLightColor, out.color);
  for (float& c : out.color) {
    c *= intensity;
  }
  if (out.kind == Kind::Omni) {
    out.nearDistance = game::field<float>(resource, game::kOmniNear);
    out.farDistance = game::field<float>(resource, game::kOmniFar);
  } else if (out.kind == Kind::Spot) {
    out.innerAngle = game::field<float>(resource, game::kSpotInner);
    out.outerAngle = game::field<float>(resource, game::kSpotOuter);
    out.nearDistance = game::field<float>(resource, game::kSpotNear);
    out.farDistance = game::field<float>(resource, game::kSpotFar);
  }
  return true;
}

void __stdcall lightUpdateHook(void* x) {
  s_originalUpdate(x);
  if (!x) {
    return;
  }
  const auto* list = game::field<const void* const*>(x, game::kVisibleLights);
  const uint32_t count = game::field<uint32_t>(x, game::kVisibleLights + 4) & 0x3FFF;
  Frame frame;
  frame.valid = true;
  std::lock_guard lock { s_mutex };
  for (uint32_t i = 0; list && i < count; ++i) {
    Light light;
    if (list[i] && readLight(list[i], light)) {
      frame.lights.push_back(light);
    }
  }
  s_frame = std::move(frame);
}

} // namespace

bool install() {
  return hook::install("Lights_Update", game::kLightUpdate, game::kLightUpdatePrologue,
                       reinterpret_cast<void*>(&lightUpdateHook), &s_originalUpdate);
}

Frame takeFrame() {
  std::lock_guard lock { s_mutex };
  Frame out = std::move(s_frame);
  s_frame = {};
  return out;
}

} // namespace ac1rtx::lights
