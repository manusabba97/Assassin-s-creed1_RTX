#include "hook.h"

#include "log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

namespace ac1rtx::hook {

bool install(const char* name, uintptr_t address, const uint8_t* prologue, size_t prologueSize,
             void* detour, void** original) {
  auto* target = reinterpret_cast<void*>(address);
  if (std::memcmp(target, prologue, prologueSize) != 0) {
    log::line("%s: prologue mismatch at 0x%08X (patched by another mod?) - NOT hooked", name,
              static_cast<unsigned>(address));
    return false;
  }
  if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
    log::line("%s: MinHook failed at 0x%08X", name, static_cast<unsigned>(address));
    return false;
  }
  log::line("%s hooked at 0x%08X", name, static_cast<unsigned>(address));
  return true;
}

} // namespace ac1rtx::hook
