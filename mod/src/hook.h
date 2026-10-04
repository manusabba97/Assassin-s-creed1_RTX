#pragma once

#include <cstddef>
#include <cstdint>

namespace ac1rtx::hook {

// Installs a MinHook detour at `address` after checking that the code there still matches `prologue`
// (other ASI mods may patch the exe). Logs and returns false on mismatch or MinHook failure.
bool install(const char* name, uintptr_t address, const uint8_t* prologue, size_t prologueSize,
             void* detour, void** original);

template<size_t N, typename Fn>
bool install(const char* name, uintptr_t address, const uint8_t (&prologue)[N], void* detour, Fn* original) {
  return install(name, address, prologue, N, detour, reinterpret_cast<void**>(original));
}

} // namespace ac1rtx::hook
