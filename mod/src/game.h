#pragma once

// Engine facts for AssassinsCreed_Dx9.exe v1.02 (MD5 8E72C3333743780E43BC2C34BBF625F9, image base 0x400000, no ASLR).
// Every address/offset below was read from the binary and confirmed live; sources are recorded in
// Vibe-Reverse-Engineering/patches/AssassinsCreed/kb.h.

#include <cstdint>

namespace game {

// Row-major 4x4, row-vector convention (p' = p * M), exactly as the engine stores it.
struct Mat4 {
  float m[16];
};

// Renderer_SetupView: __thiscall(renderer, matrixCtx, viewSurface, viewDesc), ret 0xC.
constexpr uintptr_t kSetupView = 0x00A1F100;
// Prologue at kSetupView, used to detect other mods patching the function before we hook it.
constexpr uint8_t kSetupViewPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54 };

// RenderMatrixContext layout.
constexpr uintptr_t kCtxProjection = 0x80;
constexpr uintptr_t kCtxView = 0xC0;

// scimitar::DX9ViewSurface: render target width as float (3840.0 measured for the main view at 3840x2160).
constexpr uintptr_t kViewSurfaceWidth = 0x0C;
constexpr uintptr_t kViewSurfaceVtable = 0x016BF2FC;

// DX9ViewSurface + 0x6D: occlusion-culling enable (settings struct at +0x24, default 1). FUN_00ABA580 derives
// renderer +0x62 (occlusion prepass, pass type 4: box draws inside D3DQUERYTYPE_OCCLUSION queries) and +0x68
// (colour-pass occMode 2 = skip items whose query returned <= 20 samples, DAT_0194FD1C) from it every frame.
// Under Remix the queries return 0 samples (measured: 1424 of 1490 colour-pass reads at 0xABB289 were 0), which
// removed almost every object from the colour pass (46 BeginDraw calls per frame instead of ~630 with this at 0).
constexpr uintptr_t kViewSurfaceOcclusionCulling = 0x6D;

// DX9StaticMeshInstance<StaticPrimitive>::Submit (vtable 0x016D6624 slot 1):
// __thiscall(meshInstance, renderItem, matrixCtx, placement), ret 0xC. Calls SetWorld(placement + 0x40).
constexpr uintptr_t kStaticMeshSubmit = 0x00AE03C0;
constexpr uint8_t kStaticMeshSubmitPrologue[] = { 0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5C, 0x24, 0x1C, 0x8B, 0x43, 0x1C, 0x55 };
constexpr uintptr_t kPlacementWorld = 0x40;

// DX9StaticMeshInstance<MaskedStaticPrimitive>::Submit (vtable 0x016D663C slot 1): same signature and instance
// layout; it draws cell-culled sub-ranges of the same DX9StaticMesh (FUN_00ADE5B0, ECX = [instance + 0x2C]).
// Its vertex shaders use the static-mesh decode (def c4 = 3.81481368e-6, -127, 0.00787401572; c6.x = 16).
constexpr uintptr_t kMaskedMeshSubmit = 0x00AE0830;
constexpr uint8_t kMaskedMeshSubmitPrologue[] = { 0x83, 0xEC, 0x10, 0x53, 0x8B, 0xD9, 0x0F, 0xB7, 0x4B, 0x04, 0x57 };

// Masked-mesh cells (FUN_00ADE5B0, MaskedMesh_DrawVisibleCells):
//   H = [[item+0x10]+0x60]; o1 = [H] if [H+8] & 0x10000000; H2 = [o1+0x1C] (needs [H2+8] & 0x10000000);
//   A = [[H2]+0xC]; first = ([mesh+0x1C] >> 12) & 0xFFF; n = byte [mesh+0x1F]; group = A + (first / [A+8]) * 0x14
//   (needs [group+0x10] & 0x3FFF); cells of range r = [[group+0xC] + r*8] -> n x {int start, int count} in index
//   units relative to the range startIndex. Cell k is drawn iff bit (7 - ((first+k) & 7)) of byte
//   [[item+0xC]+0x2B8] + ((first+k) >> 3) is set.
constexpr uintptr_t kRenderItemObject = 0x10;
constexpr uintptr_t kRenderItemCellOwner = 0x0C;
constexpr uintptr_t kCellVisibilityBits = 0x2B8;
constexpr uintptr_t kStaticMeshCellInfo = 0x1C;     // bits 12-23: first cell id
constexpr uintptr_t kStaticMeshCellCount = 0x1F;    // byte
constexpr uintptr_t kMeshInstanceVertexDecl = 0x28;   // IDirect3DVertexDeclaration9* passed to SetVertexDeclaration
constexpr uintptr_t kMeshInstanceStaticMesh = 0x2C;   // scimitar::DX9StaticMesh*

// Background (sky) pass: __thiscall(renderPass, list, a2, a3, a4, a5), ret 0x14. Sets the viewport depth range to
// MinZ = MaxZ = 1.0, ZWRITEENABLE 0, ZFUNC LESSEQUAL, then RenderBatch_DrawItems over the sky lists (renderer +0x308
// and +0x338, called from FUN_00AC0F30). Everything it draws is at infinite depth; the sky dome in it is a static
// mesh (512 tris) whose world translation is the camera eye every frame.
constexpr uintptr_t kSkyPass = 0x00ABB440;
constexpr uint8_t kSkyPassPrologue[] = { 0x83, 0xEC, 0x24, 0x80, 0x79, 0x5C, 0x00, 0x8B, 0x44, 0x24, 0x28 };

// MaterialInstance_EndDraw: __thiscall(matInst, uint8 record, DX9Device* device, RenderItem* item), ret 0xC.
// Called by every mesh Submit right after its DrawIndexedPrimitive, so the device wrapper's state cache still
// holds that draw's pixel shader and textures.
constexpr uintptr_t kMaterialEndDraw = 0x00AA3130;
constexpr uint8_t kMaterialEndDrawPrologue[] = { 0x0F, 0xB6, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8D, 0x34, 0x40 };

// Render item: +0x04 DX9Device wrapper, +0x14 pass type (0/1 colour pass, 2/4/5 depth/shadow; FUN_00ABB0E0).
constexpr uintptr_t kRenderItemDevice = 0x04;
constexpr uintptr_t kRenderItemPassType = 0x14;

// DX9Device wrapper state cache written by the material command-list handlers (table 0x193851C):
// SetTexture cache [stage] at +0x29D8 (FUN_00A179E0), SetPixelShader cache at +0x2998 (FUN_00A17B00).
constexpr uintptr_t kDeviceTextureCache = 0x29D8;
constexpr uintptr_t kDevicePixelShaderCache = 0x2998;
constexpr uintptr_t kDeviceVertexShaderCache = 0x2994;  // kb.h (SetVertexShader cache next to the PS one)
// Pixel shader float constants shadow: FUN_00AA9540 (the wrapper's SetPixelShaderConstantF) keeps 224 float4 at
// wrapper + 0x1010 + reg*16 and uploads only changed registers; after a draw it holds the values that draw used
// (docs/notes/specular.md section 3; the VS twin FUN_00AA93C0 uses +0x10, c120 = +0x790).
constexpr uintptr_t kDevicePixelConstants = 0x1010;
// Layered terrain/wall PS 7a87be5e (FNV-1a 64 of the bytecode below; 92 draws in frame_capture_01):
//   albedo = lerp(lerp(tex(s3)*c128, tex(s4)*c129, COLOR1.x), tex(s5)*c130, COLOR1.y) * COLOR0.rgb
//   normal = lerp(lerp(tex(s0), tex(s1), COLOR1.x), tex(s2), COLOR1.y); COLOR1 = (NORMAL.w, TANGENT.w) / 255 (VS)
// CTAB: NormalMap_0 s0, NormalLayer1_1 s1, NormalLayer2_2 s2, BaseTexture_3 s3, Layer1_4 s4, Layer2_5 s5,
// BaseColor_5 c128, Layer1Color_7 c129, Layer2Color_9 c130.
// Bytecode hashes whose layer formula was verified from their disassembly (scripts/specular/verify_layered.py over
// the PS dumped in Masyaf; 7a87be5e = frame_capture_01 census). All use the samplers above; the tint registers
// come from each variant's CTAB (36A729ED names them ColorBase_15/Colotlayer1_17/Colorlayer2_19 at c131..c133).
struct LayeredShader {
  uint64_t hash;
  int tintRegister;  // base, layer1, layer2 tints at tintRegister + 0..2
};
constexpr LayeredShader kLayeredTerrainPs[] = {
    {0xA8238DD6A74E9DC7ull, 128}, {0x21720CAD89FD54B5ull, 128}, {0x429629C6918783BEull, 128},
    {0x72A781B0AB816ADEull, 128}, {0x73397B63A98060E8ull, 128}, {0x908D198B3DC3842Bull, 128},
    {0xA2CA2175F22BF9F4ull, 128}, {0xBE06822EE3111706ull, 128}, {0xD5646358C9739030ull, 128},
    {0xE0F0CE3434064CCDull, 128}, {0xE8442E03BDBC6751ull, 128}, {0xEBD9C2D882171DC7ull, 128},
    {0xFE762CB0F2D5D5EBull, 128}, {0x36A729ED3BAFCBC1ull, 131},
};
constexpr int kLayerAlbedoSampler[2] = { 4, 5 };
constexpr int kLayerNormalSampler[2] = { 1, 2 };
// Character skin PS (census b1b370c7 / 74673c36, Masyaf dump FC750BAF; samplers named only "OperatorN_k" so the
// name-based lookup finds neither albedo nor normal). Data flow of their disassembly (identical in all three):
//   N       = (2*tex(s0) - 1) applied to T/B/N inputs, like the static colour PS (normal map s0)
//   diffuse = lerp(tex(s1), c128, tex(s2).x) * saturate(tex(s3) + c129)      (Operator3_1, Operator31_2,
//             Operator11_3; Operator30_1 c128, Operator12_5 c129); s4 = N.L wrap ramp, s5 = specular map.
//   oC0.w   = COLOR0.x (no texture alpha).
// Blended-surface PS (dumped in Masyaf, symbolic output of scripts/specular/sm3.py):
//   A 52FCDEB0 1CB3AA2F DE0BD13C E070F086: rgb = tex(s0) * DiffColor_1(c128) * COLOR0,  a = tex.a * Alphablend_5(c129.x) * COLOR0.a
//   B 1C57675B:                            rgb = tex(s0) * COLOR0 * DiffuseColor_3(c128) * max(1, DiffuseUp_4(c129.x)),
//                                          a = tex.a * Operator7_8(c130.x) * COLOR0.a
//   C 92E7071E (Operator20_1) D57C9232 (Operator1_1): rgb = tex(s0) * c128, a = tex.a (no vertex colour input)
struct BlendTintShader {
  uint64_t hash;
  int colorRegister;   // rgb factor
  int alphaRegister;   // .x alpha factor, -1 none
  int boostRegister;   // .x factor applied as max(1, x), -1 none
  bool vertexColor;    // COLOR0 multiplies rgb and alpha
};
constexpr BlendTintShader kBlendTintPs[] = {
    {0x52FCDEB0C7D03EA3ull, 128, 129, -1, true}, {0x1CB3AA2F99FC3978ull, 128, 129, -1, true},
    {0xDE0BD13C73A510C7ull, 128, 129, -1, true}, {0xE070F0861EE94541ull, 128, 129, -1, true},
    {0x1C57675BFDC3DC98ull, 128, 130, 129, true},
    {0x92E7071E7E235F77ull, 128, -1, -1, false}, {0xD57C9232AAC605B8ull, 128, -1, -1, false},
};
// Water PS AAC5D7B1 (Masyaf dump): N = two panned normal maps s0/s1 (Panner matrices c128/c131, BumpFactor c134);
// rgb = cubemap(reflect(V, N)) (s2) * CubeMapIntensity_15 (c136.x) * Color_9 (c135) lit by ambient cube + sun, plus
// Phong pow(sat(dot(reflect(-L, N), V)), Operator9_25 (c139.x)); a = Opacity_20 (c137.x) * COLOR0.a *
// max(g_LODBlendFactor (c30.x), LODBlendToggle_22 (c138.x)). No albedo texture.
struct WaterShader {
  uint64_t hash;
  int colorRegister, opacityRegister, lodFactorRegister, lodToggleRegister, phongRegister, cubeIntensityRegister;
};
constexpr WaterShader kWaterPs[] = { {0xAAC5D7B1F8A5F735ull, 135, 137, 30, 138, 139, 136} };
// Water normal-map uv = (dot((wx, wy, 1), c128.xyz), dot((wx, wy, 1), c129.xyz)) (Panner01_3_matrix rows), with
// (wx, wy) the world position from its VS 3F07D8C7 (texcoord1 = pos x g_World); NormalMap02 uses c131/c132.
constexpr int kWaterPannerRegister = 128;
// Skin PS (samplers named only OperatorN_k): every variant below disassembles to the same diffuse,
// lerp(tex(s1), c128, tex(s2).x) * saturate(tex(s3) + c129) (tests/shaders/ps_<hash>.txt); the last 9 were found
// on 2026-10-03 (NPCs with grey body parts: material created with no albedo).
constexpr uint64_t kSkinPs[] = { 0xB8F5B0DA62C2CBD4ull, 0x5352C7E6A9B1C5AFull, 0xFC750BAFDCAAF4EBull,
                                 0xB01E82CFD6390C3Full, 0x13F754806111C0C4ull, 0x2ED631BAEBA9BD3Dull,
                                 0x3E4E4E54C65B88C9ull, 0x66FE8E4705DD329Bull, 0x6B26B12B11DCF1FDull,
                                 0xAFEC50842599A937ull, 0xB7DF026655DEB65Cull, 0xE0360C47EAE13D1Bull };
// Untextured PS EEF09AA5AF65160E (samplers: none; tests/shaders/ps_EEF09AA5AF65160E.txt): diffuse = Colors_1 (c128)
// x COLOR0 under the ambient cube and direct lights; the material colour is c128.rgb.
constexpr uint64_t kConstantColorPs = 0xEEF09AA5AF65160Eull;
// Procedural smoke / mist PS 3BC141C8F719D472 (samplers Smoke2_0, HeighMap_1 scrolled by five animated 3x3 uv matrices,
// tests/shaders/ps_3BC141C8F719D472.txt): no static material can show it (drawn as a grey translucent sheet); hidden
// until particles and effects are handled.
constexpr uint64_t kHiddenEffectPs[] = { 0x3BC141C8F719D472ull };
constexpr int kConstantColorRegister = 128;
constexpr int kSkinNormalSampler = 0;
constexpr int kSkinBaseSampler = 1;
constexpr int kSkinMaskSampler = 2;
constexpr int kSkinModulateSampler = 3;
constexpr int kSkinTintRegister = 128;
constexpr int kSkinBiasRegister = 129;
constexpr uint32_t kPixelConstantCount = 224;
constexpr uint32_t kDeviceTextureStages = 16;

// Normal maps (static-mesh colour-pass PS bytecode): d = 2*rgb - 1, N_world = d.x*T + d.y*B + d.z*N with
// T = TANGENT, B = sign(POSITION.w) * cross(N, T); no swizzle, green not inverted, alpha unused. First normal sampler
// in the CTAB (NormalMap_0 / normalmap_0 / Normal_0; layered PS blend NormalLayerN on top of it). Measured over the
// static meshes: engine T follows dP/du (99.4 %), engine B is opposite to dP/dv (99.3 %). Normal maps are DXT1
// (397 of 400 draws) or DXT5.

// Material +0x10 alpha test (FUN_00A212D0): bit 24 ALPHATESTENABLE, bits 6-13 ALPHAREF, bit 5 ALPHAFUNC LESS
// (else GREATEREQUAL).
constexpr uint32_t kMaterialAlphaTest = 1u << 24;
constexpr uint32_t kMaterialAlphaFuncLess = 1u << 5;

// ---- Skinned meshes (docs/notes/skinned.md; FUN_00AE1810 SkinnedMeshInstance_Submit, vtable 0x016D66BC slot 1).
// Bone palette builder FUN_00AE11A0: __thiscall(entry, ctx, placement, palettes = instance + 0x38), ret 0xC. Called
// per draw entry right before its DrawIndexedPrimitive; the entry's MaterialInstance_EndDraw follows the draw.
constexpr uintptr_t kSkinnedPalette = 0x00AE11A0;
constexpr uint8_t kSkinnedPalettePrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xB8, 0xE4, 0x10, 0x00, 0x00 };
constexpr uintptr_t kSkinnedPalettesInInstance = 0x38;  // instance + 0x38 -> palette record array
constexpr uintptr_t kSkinnedEntrySize = 0x14;           // skinned draw entries are 0x14 bytes (static: 0x10)
constexpr uintptr_t kSkinnedEntryPalette = 0x10;        // u8 palette record index
constexpr uintptr_t kPaletteRecordSize = 0x104;         // u32 count (<= 32), Mat4* src[32], u32 poseIndex[32]
constexpr uint32_t kMaxBones = 32;
// Final bones as uploaded: device wrapper + 0x790 + i*64 + k*16 = register c(120 + 4i + k) = column k of the
// row-vector bone matrix B_i (p_world = p_model * B_i; the skinned world matrix is identity, 0x01A0F000).
constexpr uintptr_t kDeviceBoneShadow = 0x790;
// Vertex format (decls 0x21EA5F00 / 0x21EA3C40, stride 32): SHORT4N pos @0 (xyz * 16), UBYTE4 N @8, T @12, B @16
// ((b-127)/127), SHORT2N uv @20 (* 16), UBYTE4 BLENDINDICES @24 (palette slot), UBYTE4N BLENDWEIGHT @28 (the VS
// divides by the weight sum). Static-mesh position decode does NOT apply.
constexpr uint32_t kSkinnedStride = 32;
constexpr float kSkinnedPositionScale = 16.0f / 32767.0f;

// Per-instance draw list (FUN_00AE03C0 loop, LOD list 0 = main pass): { DrawEntry* entries; uint32 count & 0x3FFF }.
constexpr uintptr_t kMeshInstanceDrawEntries = 0x08;
constexpr uintptr_t kMeshInstanceDrawEntryCount = 0x0C;

// 16-byte draw entry. +0: material instance (ECX of FUN_00A4B1B0 / FUN_00A4A580);
// +0xB: bits 0-6 primitive range index, bit 7 forces a cull winding (param_3 of FUN_00A4A580).
constexpr uintptr_t kDrawEntrySize = 0x10;
constexpr uintptr_t kDrawEntryMaterialInstance = 0x00;
constexpr uintptr_t kDrawEntryRange = 0x0B;
// +0xC: bit 0 forces depth write on, bit 1 forces it off (param_4 / param_5 of FUN_00A4A580).
constexpr uintptr_t kDrawEntryDepthFlags = 0x0C;

// Material instance: +0 material, +0xC bit 0 forces a cull winding. Material flags at material + 0x10:
//   bit 26: never drawn (FUN_00A4B1B0 returns 0); bit 25: cull disabled; bit 19: cull CCW instead of CW
//   (FUN_00A4A580 -> FUN_00A21400(!bit25, bit19): D3DRS_CULLMODE = bit25 ? NONE : bit19 ? CCW : CW).
//   When either forcing flag is set the cull mode is CCW if the entry bit 7 is set, else CW.
constexpr uintptr_t kMaterialInstanceMaterial = 0x00;
constexpr uintptr_t kMaterialInstanceFlags = 0x0C;
constexpr uintptr_t kMaterialFlags = 0x10;
constexpr uint32_t kMaterialHidden = 1u << 26;
constexpr uint32_t kMaterialNoCull = 1u << 25;
constexpr uint32_t kMaterialCullCCW = 1u << 19;
// D3DRS_ZWRITEENABLE (FUN_00A4A580 -> FUN_00A24A60) = !(flags & (blend == 0 ? bit 14 : bit 15)), blend = flags & 7.
// The sky dome (placed at the eye every frame, ZWRITEENABLE 0 in frame_capture_01 seq 31269) is drawn this way.
constexpr uint32_t kMaterialBlendMask = 7u;
constexpr uint32_t kMaterialNoDepthWriteOpaque = 1u << 14;
constexpr uint32_t kMaterialNoDepthWriteBlended = 1u << 15;
// Blend mode m = flags & 7 -> FUN_00A21350: ALPHABLENDENABLE = m != 0, SRCBLEND = [0x1947AD8 + m*4],
// DESTBLEND = [0x1947AC0 + m*4] (read from the binary): 1 ONE/ONE, 2 ONE/INVSRCCOLOR, 3 DESTCOLOR/ZERO,
// 4 SRCALPHA/INVSRCALPHA, 5 DESTCOLOR/ONE, 6-7 ZERO/ONE. Masyaf colour pass (frame_capture_01): every draw without
// depth write is SRCALPHA/INVSRCALPHA, ZFUNC LESSEQUAL.
constexpr uint32_t kBlendAdditive = 1, kBlendScreen = 2, kBlendMultiply = 3, kBlendAlpha = 4, kBlendGlass = 5;
// Mode 5 (DESTCOLOR/ONE: dst * (1 + src), background never attenuated) PS, all 7 in Masyaf/Animus with the same
// graph: rgb = lit tex(s1)*DiffuseColor_1*COLOR0 + Phong (SpecPower_15 c131, SpecFactor_16 c132) *
// tex(s2)*SpecularColor_5*cubemap(s3)*Operator3_7, times lerp(Fresnel_17 (c133.x), 1, (1 - N.V)^5) - c133 is a
// specular intensity (measured 5/15/20), not a reflectance; normal map Normal_0 s0. Sent as Remix translucent.
constexpr uint64_t kGlassPs[] = { 0x0E0234C3BFCBED4Eull, 0x234EA27C6B6D50FFull, 0x4D8BCF709BCD32B7ull,
                                  0x7BA88B465A34FC0Bull, 0x7C570F1B5FB3EEE6ull, 0xA4B1908C3F5B9251ull,
                                  0xD3684DC699AB3559ull };

// Draw entry +0xA: mask of the render lists that draw it; RenderBatch_DrawItems (0xABB0E0) stores its list index in
// item +0x18 and 1 << list in item +0x1C, and Submit draws an entry only if (entry[0xA] & item[0x1C]) != 0.
// List 1 is drawn by FUN_00ABEA60 (from 0xAC1149): ZWRITE 0, ZFUNC LESSEQUAL, blending on, D3DRS_DEPTHBIAS =
// [[ctx+0x40]+0x4C] (0xB82828AC in frame_capture_01) - the decal pass (live: RenderBatch_DrawItems caller 0xABEB34,
// list 1, pass 1).
constexpr uintptr_t kDrawEntryLists = 0x0A;
constexpr uint8_t kDecalListBit = 1u << 1;

// Static vertex colour: stream 0 +16, D3DDECLUSAGE_COLOR, D3DCOLOR (decl 0x21EA65A0). The VS passes it through
// (mov o3, v3); blended PS multiply albedo by its rgb and opacity by its alpha (PS 0x22E78CC0 / 0x22E7CC80), opaque
// world PS do not read it. Stream 1 (TEXCOORD4, D3DCOLOR) is per-instance baked AO (g_AOParams), not used.

// scimitar::DX9StaticMesh destructor body (called by the deleting destructor at vtable 0x016D6614 slot 0).
constexpr uintptr_t kStaticMeshDestructor = 0x00ADEE90;
constexpr uint8_t kStaticMeshDestructorPrologue[] = { 0x6A, 0xFF, 0x68, 0xDF, 0x0B, 0x57, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00 };

// scimitar::DX9StaticMesh layout (FUN_00ADFCA0 binds stream 0 / indices; FUN_00AE03C0 issues the draws).
constexpr uintptr_t kStaticMeshPrimitiveTable = 0x04;  // PrimitiveRange*
constexpr uintptr_t kStaticMeshPrimitiveCount = 0x08;  // & 0x3FFF
constexpr uintptr_t kStaticMeshVertexBuffer = 0x14;    // DX9Device::VertexBuffer* (IDirect3DVertexBuffer9* at +8)
constexpr uintptr_t kStaticMeshIndexBuffer = 0x18;     // DX9Device::IndexBuffer* (IDirect3DIndexBuffer9* at +8)
constexpr uintptr_t kStaticMeshStride = 0x1C;          // & 0x7FF
constexpr uintptr_t kDeviceBufferD3D = 0x08;

// Arguments of DrawIndexedPrimitive(type, 0, minVertex, numVertices, startIndex, primCount).
struct PrimitiveRange {
  uint32_t type;
  uint32_t minVertex;
  uint32_t numVertices;
  uint32_t startIndex;
  uint32_t primitiveCount;
};
static_assert(sizeof(PrimitiveRange) == 0x14);

// Static-mesh vertex decoding, read from the vertex shader bytecode (CTAB g_World c8):
//   pos = v0.xyz * |v0.w * 3.81481368e-6| ; normal = (ubyte - 127) * 0.00787401572 ; uv = short2n * 16
constexpr float kPositionScale = 3.81481368e-6f;
constexpr float kNormalBias = 127.0f;
constexpr float kNormalScale = 0.00787401572f;
constexpr float kTexcoordScale = 16.0f;

// ---- Lights (phase 9) ----
// FUN_00AA1AD0: __stdcall(X), ret 4, X = [view + 0x1E0]; calls FUN_00AA19B0(X + 0xA40, X + 0xA4C), which walks the
// visible light objects (FUN_00AA14B0) and relights the queued objects (FUN_00AA1770 -> FUN_00AD5570).
// Live (Masyaf): called every frame with the same X; X + 0xA4C = {LightObject**, count & 0x3FFF}.
constexpr uintptr_t kLightUpdate = 0x00AA1AD0;
constexpr uint8_t kLightUpdatePrologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x8D, 0x90, 0x4C, 0x0A, 0x00, 0x00 };
constexpr uintptr_t kVisibleLights = 0xA4C;

// DX9GraphicObjectInstance (the `placement` of the Submit hooks, and each light object L):
//   +0x24: bits 0-2 object type (1 = light, FUN_00AA18F0), bits 3-10 light channels.
// A light lights an object only when their channel masks intersect (FUN_00AA0B40: (mask & (obj+0x24 >> 3)) == 0
// rejects). A light's channels come from its resource flags R+0x110 (0xAA2394: bit3 -> ch 0x01, bit2 -> 0x02,
// bit4 -> 0x04, bit5 -> 0x08, bits6/7 -> 0x20, bit8 -> 0x40). Live Masyaf: world objects ch 0x01 (1006 of 1076
// queued), characters ch 0x02 (62); SunLight ch 0x01 lights the world, a second SunLight ch 0x02 (0.85 grey, custom
// specular direction) lights only the skinned draws (343 of 399 in frame_capture_01).
constexpr uintptr_t kObjectFlags = 0x24;
constexpr uint32_t kObjectTypeMask = 0x7;
constexpr uint32_t kObjectTypeLight = 1;
constexpr uint32_t kObjectChannelShift = 3;
constexpr uint32_t kObjectChannelMask = 0xFF;
constexpr uintptr_t kLightAxisY = 0x50;     // world +Y row: spot and directional lights shine along it (FUN_00AD4010)
constexpr uintptr_t kLightPosition = 0x70;  // world position row
constexpr uintptr_t kLightResource = 0xA4;  // scimitar::Light*

// scimitar::Light resource (serializer FUN_00A05D80): +0x10 intensity, +0x100 colour rgb.
// Colour sent to the shaders = rgb * intensity * fade/255 (FUN_00AD49C0 / FUN_00AD4010); fade is the per-object
// priority fade of the selection, not a property of the light.
constexpr uintptr_t kLightIntensity = 0x10;
constexpr uintptr_t kLightColor = 0x100;
// Class by vtable (RTTI).
constexpr uintptr_t kOmniLightVtable = 0x016BE5F4;         // +0x120 near, +0x124 far (FUN_00A065B0)
constexpr uintptr_t kSpotLightVtable = 0x016BE5B4;         // +0x120 inner, +0x124 outer half-angle (rad), +0x12C near, +0x130 far
constexpr uintptr_t kDirectionalLightVtable = 0x016BE634;
constexpr uintptr_t kSunLightVtable = 0x016BE6B4;
constexpr uintptr_t kOmniNear = 0x120, kOmniFar = 0x124;
constexpr uintptr_t kSpotInner = 0x120, kSpotOuter = 0x124, kSpotNear = 0x12C, kSpotFar = 0x130;

// ---- Dynamic sub-meshes (cloth: robes, awnings) ----
// scimitar::DX9DynamicSubMeshInstance (vtable 0x016C2DF4) slot 1 Submit: __thiscall(instance, item, ctx, placement).
// Draw entries: instance +0x08 ptr, +0x0C count & 0x3FFF, 12 bytes each: +0 MaterialInstance*, +4 record byte per
// pass, +0xA render-list mask, +0xB bit0 forced winding (CCW when set), bit1 depth write on, bit2 depth write off
// (FUN_00A4B1B0 args at 0xAA3B9x). instance +0x1C: dynamic mesh R; +0x24 vertex decl; +0x30 bone count: 0 = CPU
// vertex path (cloth, world = item + 0x100 set at 0xAA3ECC), != 0 = instanced path (clutter).
// Instanced path (decompiled Submit): +0x30 = instance count N, +0x48 -> N 4x4 matrices (0x40 bytes each).
// Type [instance+0x20] & 7 of 5 or 6 (clutter): g_World = identity, then batches of 32 instances whose matrices are
// uploaded raw (FUN_00AA93C0, no transpose: register r = world output component r) to c120 g_ClutterWorldMatrices;
// each batch draws R+0x28 * n / 32 primitives of a VB holding 32 copies of the mesh, TEXCOORD3 UBYTE4 .x = copy index
// (VS 0x215A24C0: mova a0.x, 4 * v1.x; frame_capture_01: 21 draws, decl stride 40). When the type-5 path finds a
// terrain the batch is 128 with 16-byte records instead (not seen in Masyaf). Other types: one draw per instance
// with g_World (0xEA via FUN_00A21510, which transposes like every world matrix) = matrix i.
constexpr uintptr_t kDynamicType = 0x20, kDynamicMatrices = 0x48;
constexpr uint32_t kClutterBatch = 32;
constexpr uintptr_t kDynamicSubmit = 0x00AA38A0;
constexpr uint8_t kDynamicSubmitPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68 };
constexpr uintptr_t kDynamicEntries = 0x08, kDynamicEntryCount = 0x0C, kDynamicEntrySize = 0x0C;
constexpr uintptr_t kDynamicEntryFlags = 0x0B;
constexpr uintptr_t kDynamicMesh = 0x1C, kDynamicDecl = 0x24, kDynamicBoneCount = 0x30;
constexpr uintptr_t kItemWorld = 0x100;
// Dynamic mesh draw FUN_00AA4780: __thiscall(R, device**, primCount), ret 8. FUN_00A0CAD0 copies [R+0x40] bytes from
// the CPU vertex array [R+0x3C] into a ring VB, stream 0 stride [R+0x2C]; IB wrapper [R + 0x10 + [R+0x20]*4]
// (D3D IB at +8); DrawIndexedPrimitive(type [R+0x24], 0, 0, [R+0x30] vertices, 0, min(primCount, [R+0x28])).
// Cloth decl (frame_capture_01 seq 17825): FLOAT3 pos @0, FLOAT3 normal @12, FLOAT2 uv @24, stride 32.
constexpr uintptr_t kDynamicDraw = 0x00AA4780;
constexpr uint8_t kDynamicDrawPrologue[] = { 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x3C, 0x8B, 0x4E, 0x40 };
constexpr uintptr_t kDynIndexBuffers = 0x10, kDynIndexBufferSlot = 0x20, kDynPrimType = 0x24, kDynMaxPrims = 0x28;
constexpr uintptr_t kDynStride = 0x2C, kDynVertexCount = 0x30, kDynCpuVertices = 0x3C, kDynCpuBytes = 0x40;

template<typename T>
inline T field(const void* object, uintptr_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
}

inline const Mat4& ctxProjection(const void* ctx) {
  return *reinterpret_cast<const Mat4*>(static_cast<const uint8_t*>(ctx) + kCtxProjection);
}

inline const Mat4& ctxView(const void* ctx) {
  return *reinterpret_cast<const Mat4*>(static_cast<const uint8_t*>(ctx) + kCtxView);
}

inline float viewSurfaceWidth(const void* surface) {
  return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(surface) + kViewSurfaceWidth);
}

} // namespace game
