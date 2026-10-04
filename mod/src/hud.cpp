#include "hud.h"

#include "hook.h"
#include "log.h"
#include "materials.h"

#include <windows.h>

#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace ac1rtx::hud {

namespace {

// FUN_009ED5F0, cdecl(void* draw2d, void* frameParams), called only from FUN_00A1F5A0 (0xA1F5D7), where
// draw2d = FUN_00A0C9F0(...) = device wrapper + 0x2B30 (the engine's 2D drawing interface) and frameParams is the
// frame descriptor (+0xC/+0x10 width/height, +0x46 skips the overlay). It runs the callbacks registered through
// FUN_009ED380 ({fn, ctx, extra} at [0x01A255D8], count [0x01A255DC] & 0x3FFF): in game one, 0x786570 (ctx
// 0x01A135A0), which draws the triple-buffered 2D command list (FUN_00786180); the others are the SoftBody/FX/
// Physic stats overlays and the video callback 0xA63F70.
// FUN_00A1F5A0 is the first step of the renderer's EndFrame FUN_00A4C510 and runs after all views (FUN_00A1F4A0);
// only viewport reset and EndScene follow, then Present (FUN_00A0C380, called only from FUN_00A4C510).
// frame_capture_01 (both frames): every draw after the dispatcher's first draw returns through 0x9ED656 (181/181),
// none before it.
constexpr uintptr_t kOverlayDispatch = 0x009ED5F0;
// push -1 ; push 0x01565528 ; mov eax, fs:[0]
constexpr uint8_t kOverlayDispatchPrologue[] = { 0x6A, 0xFF, 0x68, 0x28, 0x55, 0x56, 0x01,
                                                 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };

using OverlayDispatchFn = void(__cdecl*)(void* draw2d, void* frameParams);
OverlayDispatchFn s_originalDispatch = nullptr;
void (*s_onOverlayBegin)() = nullptr;
void (*s_onOverlayEnd)() = nullptr;

void __cdecl overlayDispatchHook(void* draw2d, void* frameParams) {
  s_onOverlayBegin();
  s_originalDispatch(draw2d, frameParams);
  s_onOverlayEnd();
}

// ---- Post-process chain (frame_capture_01, frame 0): after the views, three DrawPrimitiveUP:
//   seq 33853 PS 0x14E043A0 (texld oC0, s0: copy) scene tex 0x364B10C0 -> RT A 0x21719220
//   seq 33871 PS 0x14C9BA80 (colour grading: oC0 = tex3D(s1 volume LUT, tex(s0) * g_PreLutScale c0 +
//             g_PreLutOffset c1)) tex 0x36514740 -> RT B 0x217194A0
//   seq 33885 PS 0x14E043A0 (copy) tex 0x36514880 -> back buffer 0x23659420 (FUN_00A1F4A0, 0xA1F4FE)
// Textures 0x36514740/0x36514880 and surfaces 0x21719220/0x217194A0 are the A/B ping-pong pair (the final copy reads
// the LUT's output B). Both the A copy and the LUT run inside FUN_00AAF540 called from 0xAC02E1 (thiscall, 4 args,
// ret 0x10; the SetRenderTarget backtraces of both pass through 0xAC02E6); no other draw precedes the copy in it.
// Marking the injection there with skipDraw makes Remix write the ray-traced image into A instead of the copy, so
// the game's own LUT and final copy run on it.
constexpr uintptr_t kPostProcessChain = 0x00AAF540;
// mov eax, fs:[0] ; push -1 ; push 0x0156F301
constexpr uint8_t kPostProcessChainPrologue[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF,
                                                  0x68, 0x01, 0xF3, 0x56, 0x01 };
constexpr uintptr_t kPostProcessChainReturn = 0x00AC02E6;

using PostProcessChainFn = uint32_t(__thiscall*)(void* self, void* a1, void* a2, void* a3, uint32_t a4);
PostProcessChainFn s_originalChain = nullptr;
void (*s_onPostProcessBegin)() = nullptr;

// Diagnostic: the chain runs optional effects in this order, each gated by the enabled() virtual (vtable +0x30) of
// an object at settings + offset, settings = [[self + 0x10] + 0x1D0] (decompiled FUN_00AAF540). Logs the enabled set
// whenever it changes.
constexpr uint32_t kEffectOffsets[] = { 0x450, 0x24, 0x44, 0x3F0, 0x140, 0x16C, 0x220, 0x188, 0x23C, 0x244,
                                        0x308, 0x35C, 0x250, 0x2AC, 0x3C8, 0x458, 0x474, 0x3B0, 0x3BC };
using EnabledFn = char(__thiscall*)(void* effect);

void logEnabledEffects(void* self) {
  const auto* renderer = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(self) + 0x10);
  auto* settings = renderer ? *reinterpret_cast<uint8_t* const*>(renderer + 0x1D0) : nullptr;
  if (!settings) {
    return;
  }
  static uint32_t s_lastMask = 0xFFFFFFFF;
  static bool s_loggedTargets = false;
  uint32_t mask = 0;
  for (size_t i = 0; i < std::size(kEffectOffsets); ++i) {
    void* effect = settings + kEffectOffsets[i];
    const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(effect);
    const auto enabled = reinterpret_cast<EnabledFn>(vtable[0x30 / 4]);
    if (!s_loggedTargets) {
      log::line("pp effect +0x%03X: vtable %08X enabled() %08X", kEffectOffsets[i],
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(vtable)), static_cast<unsigned>(vtable[0x30 / 4]));
    }
    mask |= enabled(effect) ? (1u << i) : 0u;
  }
  s_loggedTargets = true;
  if (mask != s_lastMask) {
    s_lastMask = mask;
    char text[256] = {};
    size_t n = 0;
    for (size_t i = 0; i < std::size(kEffectOffsets) && n + 8 < sizeof(text); ++i) {
      if (mask & (1u << i)) {
        n += std::snprintf(text + n, sizeof(text) - n, "+0x%03X ", kEffectOffsets[i]);
      }
    }
    log::line("pp effects enabled: %s", text);
  }
}

uint32_t __fastcall postProcessChainHook(void* self, void* /*edx*/, void* a1, void* a2, void* a3, uint32_t a4) {
  if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kPostProcessChainReturn) {
    logEnabledEffects(self);
  }
  if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kPostProcessChainReturn) {
    s_onPostProcessBegin();
  }
  return s_originalChain(self, a1, a2, a3, a4);
}

// ---- Surfaces showing another view's output (Animus menu band: mesh 7E9C1D10 range 3, render list 2, colour texture
// = the 2048x512 A8R8G8B8 render target the 2048-wide view draws). The CPU texture upload cannot read a render target,
// so the band is rasterized: the injection is marked right before its draw. It is the last transparent-list draw
// of the scene view, followed only by the post-process chain and the overlay.
// MaterialInstance_BeginDraw FUN_00A4B1B0: thiscall(matInst, item, range, forcedCCW, a4, a5), ret 0x14; applies the
// material states right before the entry's draw; returns 0 (no draw) for materials flagged never drawn (bit 26).
constexpr uintptr_t kMaterialBeginDraw = 0x00A4B1B0;
// push ebp ; mov ebp, esp ; and esp, -16 ; sub esp, 0x94
constexpr uint8_t kMaterialBeginDrawPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
constexpr uintptr_t kRenderItemPassType = 0x14;  // 0/1 colour pass (kb.h)

using BeginDrawFn = char(__thiscall*)(void* matInst, void* item, uint32_t range, char forcedCCW, char a4, char a5);
BeginDrawFn s_originalBeginDraw = nullptr;
void (*s_onRenderTargetDraw)() = nullptr;

char __fastcall beginDrawHook(void* matInst, void* /*edx*/, void* item, uint32_t range, char forcedCCW, char a4,
                              char a5) {
  const char drawn = s_originalBeginDraw(matInst, item, range, forcedCCW, a4, a5);
  if (drawn && item && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(item) + kRenderItemPassType) <= 1 &&
      materials::drawsRenderTarget(matInst)) {
    s_onRenderTargetDraw();
  }
  return drawn;
}

} // namespace

// ---- Game bloom (post-process effect settings +0x450, FUN_00ADC7D0, called at 0xAAFA54 with ecx = chain + 0x41F0):
// downsamples the chain input and a second render target ([bloom + 4], filled by the scene view's draws), multiplies
// them and adds a 5-level blurred pyramid onto the output. Remix never fills that second target, so its stale content
// shows as white halos wherever the ray-traced image is bright (Animus lab, 2026-10-01). Its gate at 0xAAFA37
// (je 0xAAFA95 when enabled() returns 0) is made unconditional: the chain then runs exactly as with bloom disabled
// (no ping-pong swap, the LUT reads the injected image). Remix's own bloom applies to the ray-traced image.
constexpr uintptr_t kBloomGate = 0x00AAFA37;
constexpr uint8_t kBloomGateBytes[] = { 0x74, 0x5C };  // je +0x5C
constexpr uint8_t kBloomGateSkip = 0xEB;               // jmp +0x5C

bool disableGameBloom() {
  auto* code = reinterpret_cast<uint8_t*>(kBloomGate);
  if (std::memcmp(code, kBloomGateBytes, sizeof(kBloomGateBytes)) != 0) {
    log::line("game bloom gate at 0x%08X does not match - bloom left on", static_cast<unsigned>(kBloomGate));
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(code, 1, PAGE_EXECUTE_READWRITE, &old)) {
    return false;
  }
  code[0] = kBloomGateSkip;
  VirtualProtect(code, 1, old, &old);
  FlushInstructionCache(GetCurrentProcess(), code, 1);
  log::line("game bloom disabled (0x%08X)", static_cast<unsigned>(kBloomGate));
  return true;
}

bool install(void (*onOverlayBegin)(), void (*onOverlayEnd)(), void (*onPostProcessBegin)(),
             void (*onRenderTargetDraw)()) {
  disableGameBloom();
  s_onRenderTargetDraw = onRenderTargetDraw;
  hook::install("MaterialInstance_BeginDraw", kMaterialBeginDraw, kMaterialBeginDrawPrologue,
                reinterpret_cast<void*>(&beginDrawHook), &s_originalBeginDraw);
  s_onOverlayBegin = onOverlayBegin;
  s_onOverlayEnd = onOverlayEnd;
  s_onPostProcessBegin = onPostProcessBegin;
  const bool overlay = hook::install("Overlay_Dispatch", kOverlayDispatch, kOverlayDispatchPrologue,
                                     reinterpret_cast<void*>(&overlayDispatchHook), &s_originalDispatch);
  const bool chain = hook::install("PostProcess_Chain", kPostProcessChain, kPostProcessChainPrologue,
                                   reinterpret_cast<void*>(&postProcessChainHook), &s_originalChain);
  return overlay && chain;
}

} // namespace ac1rtx::hud
